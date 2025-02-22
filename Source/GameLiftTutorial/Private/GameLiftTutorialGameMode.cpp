// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "GameLiftTutorialGameMode.h"
#include "GameLiftTutorial.h"
#include "TextReaderComponent.h"
#include "Engine/Engine.h"
#include "GameLiftTutorialCharacter.h"
#include "GameLiftTutorialPlayerState.h"
#include "GameLiftTutorialHUD.h"
#include "UObject/ConstructorHelpers.h"
#include "Kismet/GameplayStatics.h"
#include "Json.h"
#include "JsonUtilities.h"
#include "GameFramework/GameSession.h"
#if WITH_GAMELIFT
#include "GameLiftServerSDK.h"
#endif

AGameLiftTutorialGameMode::AGameLiftTutorialGameMode()
	: Super()
{
	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPersonCPP/Blueprints/ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != NULL)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
		PlayerStateClass = AGameLiftTutorialPlayerState::StaticClass();
		HUDClass = AGameLiftTutorialHUD::StaticClass();
	}

	StartGameSessionState = new FStartGameSessionState();
	UpdateGameSessionState = new FUpdateGameSessionState();
	ProcessTerminateState = new FProcessTerminateState();
	HealthCheckState = new FHealthCheckState();

	UTextReaderComponent* TextReader = CreateDefaultSubobject<UTextReaderComponent>(TEXT("TextReaderComp"));
	ApiUrl = TextReader->ReadFile("SecretUrls/ApiUrl.txt");
	AssignMatchResultsUrl = ApiUrl + "/assignmatchresults";

	NumTimesFoundNoPlayers = 0;
	GameStarted = false;
	HttpModule = &FHttpModule::Get();
	
	ServerPassword = "";
}

void AGameLiftTutorialGameMode::Logout(AController* Exiting) {
	Super::Logout(Exiting);
	APlayerState* State = Exiting->PlayerState;
	if (State != nullptr) {
		AGameLiftTutorialPlayerState* PlayerState = Cast<AGameLiftTutorialPlayerState>(State);
		const FString& PlayerSessionId = PlayerState->PlayerSessionId;
		if (PlayerSessionId.Len() > 0) {
#if WITH_GAMELIFT
			Aws::GameLift::Server::RemovePlayerSession(TCHAR_TO_ANSI(*PlayerSessionId));
#endif
		}
	}
	

}

void AGameLiftTutorialGameMode::BeginPlay() {
	Super::BeginPlay();
	//Let's run this code only if GAMELIFT is enabled. Only with Server targets!
#if WITH_GAMELIFT

	auto InitSDKOutcome = Aws::GameLift::Server::InitSDK();

	if (InitSDKOutcome.IsSuccess()) {
		Aws::GameLift::Server::StartGameSessionFn OnStartGameSession = [](Aws::GameLift::Server::Model::GameSession GameSessionObj, void* Params)
		{
			FStartGameSessionState* State = (FStartGameSessionState*)Params;

			State->Status = Aws::GameLift::Server::ActivateGameSession().IsSuccess();

			//extract matchmaker data 
			FString MatchmakerData = GameSessionObj.GetMatchmakerData();
			UE_LOG(LogTemp, Warning, TEXT("matchmaker data in onstartgamesession: %s"), *(MatchmakerData));

			// Create a pointer to hold the json serialized data
			TSharedPtr<FJsonObject> JsonObject;

			//Create a reader pointer to read the json data
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MatchmakerData);

			//Deserialize the json data given Reader and the actual object to deserialize
			if (FJsonSerializer::Deserialize(Reader, JsonObject))
			{
				FString LatestBackfillTicketId = JsonObject->GetStringField("autoBackfillTicketId");
				State->LatestBackfillTicketId = LatestBackfillTicketId;
				TArray<TSharedPtr<FJsonValue>> Teams = JsonObject->GetArrayField("teams");
				for (TSharedPtr<FJsonValue> Team : Teams) {
					TSharedPtr<FJsonObject> TeamObj = Team->AsObject();
					FString TeamName = TeamObj->GetStringField("name");
					TArray<TSharedPtr<FJsonValue>> Players = TeamObj->GetArrayField("players");

					for (TSharedPtr<FJsonValue> Player : Players) {
						TSharedPtr<FJsonObject> PlayerObj = Player->AsObject();
						FString PlayerId = PlayerObj->GetStringField("playerId");
						State->PlayerIdToTeam.Add(PlayerId, TeamName);
					}
				}
			}
		};

		Aws::GameLift::Server::UpdateGameSessionFn OnUpdateGameSession = [](Aws::GameLift::Server::Model::UpdateGameSession UpdateGameSessionObj, void* Params)
		{
			FUpdateGameSessionState* State = (FUpdateGameSessionState*)Params;

			Aws::GameLift::Server::Model::UpdateReason Reason = UpdateGameSessionObj.GetUpdateReason();

			if (Reason == Aws::GameLift::Server::Model::UpdateReason::MATCHMAKING_DATA_UPDATED) {
				// extract matchmaker data
				Aws::GameLift::Server::Model::GameSession GameSessionObj = UpdateGameSessionObj.GetGameSession();
				FString MatchmakerData = GameSessionObj.GetMatchmakerData();
				UE_LOG(LogTemp, Warning, TEXT("matchmaker data in onupdategamesession: %s"), *(MatchmakerData));

				// Create a pointer to hold the json serialized data
				TSharedPtr<FJsonObject> JsonObject;

				//Create a reader pointer to read the json data
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MatchmakerData);

				//Deserialize the json data given Reader and the actual object to deserialize
				if (FJsonSerializer::Deserialize(Reader, JsonObject))
				{
					TArray<TSharedPtr<FJsonValue>> Teams = JsonObject->GetArrayField("teams");
					for (TSharedPtr<FJsonValue> Team : Teams) {
						TSharedPtr<FJsonObject> TeamObj = Team->AsObject();
						FString TeamName = TeamObj->GetStringField("name");
						TArray<TSharedPtr<FJsonValue>> Players = TeamObj->GetArrayField("players");

						for (TSharedPtr<FJsonValue> Player : Players) {
							TSharedPtr<FJsonObject> PlayerObj = Player->AsObject();
							FString PlayerId = PlayerObj->GetStringField("playerId");
							State->PlayerIdToTeam.Add(PlayerId, TeamName);
						}
					}
				}
			}
			else if (Reason == Aws::GameLift::Server::Model::UpdateReason::BACKFILL_FAILED || Reason == Aws::GameLift::Server::Model::UpdateReason::BACKFILL_TIMED_OUT || Reason == Aws::GameLift::Server::Model::UpdateReason::BACKFILL_CANCELLED) {
				// clear timer handles, and terminate the game session
			}
		
			State->LatestBackfillTicketId = UpdateGameSessionObj.GetBackfillTicketId();
		};

		Aws::GameLift::Server::ProcessTerminateFn OnProcessTerminate = [](void* Params)
		{
			FProcessTerminateState* State = (FProcessTerminateState*)Params;

			State->Status = true;
		};

		Aws::GameLift::Server::HealthCheckFn OnHealthCheck = [](void* Params)
		{
			FHealthCheckState* State = (FHealthCheckState*)Params;
			State->Status = true;

			return State->Status;
		};

		TArray<FString> CommandLineTokens;
		TArray<FString> CommandLineSwitches;
		//UE_LOG(LogTemp, Warning, TEXT("Command line arguments when starting the game: %s"), *(FString(FCommandLine::Get())));
		int Port = FURL::UrlConfig.DefaultPort;

		FCommandLine::Parse(FCommandLine::Get(), CommandLineTokens, CommandLineSwitches);
		
		for (FString Str : CommandLineSwitches)
		{
			//UE_LOG(LogTemp, Warning, TEXT("Attempt to extract port from command line arguments: %s"), *(Str));
			FString Name;
			FString Value;

			if (Str.Split("=", &Name, &Value)) {
				if (Name.Equals("port")) {
					Port = FCString::Atoi(*Value);
				}
				else if (Name.Equals("password")) {
					ServerPassword = Value;
				}
			}
		}	

		const char* LogFile = "aLogFile.txt";
		const char** LogFiles = &LogFile;
		const Aws::GameLift::Server::LogParameters* LogParams = new Aws::GameLift::Server::LogParameters(LogFiles, 1);

		const Aws::GameLift::Server::ProcessParameters* Params =
			new Aws::GameLift::Server::ProcessParameters(
				OnStartGameSession,
				StartGameSessionState,
				OnUpdateGameSession,
				UpdateGameSessionState,
				OnProcessTerminate,
				ProcessTerminateState,
				OnHealthCheck,
				HealthCheckState,
				Port,
				*LogParams
			);

		auto ProcessReadyOutcome = Aws::GameLift::Server::ProcessReady(*Params);

		if (ProcessReadyOutcome.IsSuccess()) {

		}
		else {

		}
	}
	else {

	}
#endif
	GetWorldTimerManager().SetTimer(CheckPlayerCountHandle, this, &AGameLiftTutorialGameMode::CheckPlayerCount, 5.0f, true, 5.0f);
}

FString AGameLiftTutorialGameMode::InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal) {
	FString InitializedString = Super::InitNewPlayer(NewPlayerController, UniqueId, Options, Portal);
	UE_LOG(LogTemp, Warning, TEXT("inside init new player"));
	if (*Options && Options.Len() > 0) {
		const FString& PlayerSessionId = UGameplayStatics::ParseOption(Options, "PlayerSessionId");
		const FString& PlayerId = UGameplayStatics::ParseOption(Options, "PlayerId");
		UE_LOG(LogTemp, Warning, TEXT("Player session id in init new player: %s"), *(PlayerSessionId));
		if (PlayerSessionId.Len() > 0) {
#if WITH_GAMELIFT
			auto AcceptPlayerSessionOutcome = Aws::GameLift::Server::AcceptPlayerSession(TCHAR_TO_ANSI(*PlayerSessionId));
			if (AcceptPlayerSessionOutcome.IsSuccess()) {
				APlayerState* State = NewPlayerController->PlayerState;
				if (State != nullptr) {
					AGameLiftTutorialPlayerState* PlayerState = Cast<AGameLiftTutorialPlayerState>(State);
					PlayerState->PlayerSessionId = *PlayerSessionId;
					UE_LOG(LogTemp, Warning, TEXT("state is not null in init new player"));

					// assign player's mesh color based on the player's team
					if (UpdateGameSessionState != nullptr && UpdateGameSessionState->PlayerIdToTeam.Num() > 0) {
						UE_LOG(LogTemp, Warning, TEXT("Updategamesessionstate is not null and playeridtoteam is populated"));
						if (UpdateGameSessionState->PlayerIdToTeam.Contains(PlayerId)) {
							FString* Team = UpdateGameSessionState->PlayerIdToTeam.Find(PlayerId);
							PlayerState->Team = *Team;
						}
					}
					else if (StartGameSessionState != nullptr && StartGameSessionState->PlayerIdToTeam.Num() > 0) {
						UE_LOG(LogTemp, Warning, TEXT("StartGameSessionState is not null and playeridtoteam is populated"));

						if (StartGameSessionState->PlayerIdToTeam.Contains(PlayerId)) {
							FString* Team = StartGameSessionState->PlayerIdToTeam.Find(PlayerId);
							PlayerState->Team = *Team;
						}
					}
				}
			}
			else {
				UE_LOG(LogTemp, Warning, TEXT("Kicked an unauthorized player out of the game"));
				FText KickReason = FText::FromString("Unauthorized");
				// kick the player out because the player most likely did not pass a valid player session id
				GameSession->KickPlayer(NewPlayerController, KickReason);
			}
#endif
		}
	}
	return InitializedString;
}

void AGameLiftTutorialGameMode::CheckPlayerCount() {
	int NumPlayers = GetNumPlayers();
	//GEngine->AddOnScreenDebugMessage(-1, 2.f, FColor::Green, FString("Number of players in the game: ") + FString::FromInt(NumPlayers));
	if (!GameStarted && NumPlayers >= 4) {
		NumTimesFoundNoPlayers = 0;
		// "start" the game
		GetWorldTimerManager().SetTimer(StopBackfillHandle, this, &AGameLiftTutorialGameMode::StopBackfill, 1.0f, false, 15.0f);
		GetWorldTimerManager().SetTimer(EndGameHandle, this, &AGameLiftTutorialGameMode::EndGame, 1.0f, false, 30.0f);

		GameStarted = true;
	}
	else if (NumPlayers == 0) {
		NumTimesFoundNoPlayers++;

		if (NumTimesFoundNoPlayers == 10) {
			GetWorldTimerManager().ClearTimer(CheckPlayerCountHandle);
			GetWorldTimerManager().ClearTimer(StopBackfillHandle);
			GetWorldTimerManager().ClearTimer(EndGameHandle);
			// terminate the game because there is no one left, which means there is no backfill ticket
#if WITH_GAMELIFT
			auto TerminateGameSessionOutcome = Aws::GameLift::Server::TerminateGameSession();
			if (TerminateGameSessionOutcome.IsSuccess()) {
				auto ProcessEndingOutcome = Aws::GameLift::Server::ProcessEnding();
				if (ProcessEndingOutcome.IsSuccess())
				{
					FGenericPlatformMisc::RequestExit(false);
				}
			}
#endif
		}
	}
	else {
		NumTimesFoundNoPlayers = 0;
	}
}

void AGameLiftTutorialGameMode::StopBackfill() {
	//GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Green, FString("Backfill stopped"));

	FString BackfillTicketId = UpdateGameSessionState->LatestBackfillTicketId;

	if (BackfillTicketId.Len() > 0) {
#if WITH_GAMELIFT
		Aws::GameLift::Server::Model::StopMatchBackfillRequest StopBackfillRequest;
		StopBackfillRequest.SetTicketId(TCHAR_TO_ANSI(*BackfillTicketId));

		Aws::GameLift::Server::StopMatchBackfill(StopBackfillRequest);
#endif
	}

	GetWorldTimerManager().ClearTimer(StopBackfillHandle);
}

void AGameLiftTutorialGameMode::EndGame() {
	GetWorldTimerManager().ClearTimer(CheckPlayerCountHandle);
	GetWorldTimerManager().ClearTimer(EndGameHandle);

	int Num = FMath::RandRange(0, 1);
	FString WinningTeam;

	if (Num == 0) {
		// cowboys win
		WinningTeam = "cowboys";
	}
	else {
		// aliens win
		WinningTeam = "aliens";
	}
#if WITH_GAMELIFT
	TSharedPtr<FJsonObject> RequestObj = MakeShareable(new FJsonObject);
	RequestObj->SetStringField("winningTeam", WinningTeam);
	auto GameSessionIdOutcome = Aws::GameLift::Server::GetGameSessionId();
	if (GameSessionIdOutcome.IsSuccess()) {
		RequestObj->SetStringField("gameSessionId", GameSessionIdOutcome.GetResult());
	}
	else {
		
		auto TerminateGameSessionOutcome = Aws::GameLift::Server::TerminateGameSession();
		if (TerminateGameSessionOutcome.IsSuccess()) {
			auto ProcessEndingOutcome = Aws::GameLift::Server::ProcessEnding();
			if (ProcessEndingOutcome.IsSuccess())
			{
				FGenericPlatformMisc::RequestExit(false);
			}
		}
	}

	FString RequestBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestBody);

	if (FJsonSerializer::Serialize(RequestObj.ToSharedRef(), Writer)) {
		// send a get request to google discovery document to retrieve endpoints
		TSharedRef<IHttpRequest> AssignMatchResultsRequest = HttpModule->CreateRequest();
		AssignMatchResultsRequest->OnProcessRequestComplete().BindUObject(this, &AGameLiftTutorialGameMode::OnAssignMatchResultsResponseReceived);
		AssignMatchResultsRequest->SetURL(AssignMatchResultsUrl);
		AssignMatchResultsRequest->SetVerb("POST");
		AssignMatchResultsRequest->SetHeader("Authorization", ServerPassword);
		AssignMatchResultsRequest->SetContentAsString(RequestBody);
		AssignMatchResultsRequest->ProcessRequest();
	}
	else {
		auto TerminateGameSessionOutcome = Aws::GameLift::Server::TerminateGameSession();
		if (TerminateGameSessionOutcome.IsSuccess()) {
			auto ProcessEndingOutcome = Aws::GameLift::Server::ProcessEnding();
			if (ProcessEndingOutcome.IsSuccess())
			{
				FGenericPlatformMisc::RequestExit(false);
			}
		}
	}
#endif
	
}

void AGameLiftTutorialGameMode::OnAssignMatchResultsResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful) {
#if WITH_GAMELIFT
	auto TerminateGameSessionOutcome = Aws::GameLift::Server::TerminateGameSession();
	if (TerminateGameSessionOutcome.IsSuccess()) {
		auto ProcessEndingOutcome = Aws::GameLift::Server::ProcessEnding();
		if (ProcessEndingOutcome.IsSuccess())
		{
			FGenericPlatformMisc::RequestExit(false);
		}
	}
#endif
}

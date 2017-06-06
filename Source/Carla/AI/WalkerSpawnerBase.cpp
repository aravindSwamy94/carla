// CARLA, Copyright (C) 2017 Computer Vision Center (CVC)

#include "Carla.h"
#include "WalkerSpawnerBase.h"

#include "Components/BoxComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"

#include "WalkerAIController.h"
#include "WalkerSpawnPoint.h"

// =============================================================================
// -- Static local methods -----------------------------------------------------
// =============================================================================

static bool WalkerIsValid(ACharacter *Walker)
{
  return ((Walker != nullptr) && !Walker->IsPendingKill());
}

static AWalkerAIController *GetController(ACharacter *Walker)
{
  return (WalkerIsValid(Walker) ? Cast<AWalkerAIController>(Walker->GetController()) : nullptr);
}

static float GetDistance(const FVector &Location0, const FVector &Location1)
{
  return FMath::Abs((Location0 - Location1).Size());
}

static float GetDistance(const AActor &Actor0, const AActor &Actor1)
{
  return GetDistance(Actor0.GetActorLocation(), Actor1.GetActorLocation());
}

// =============================================================================
// -- Constructor and destructor -----------------------------------------------
// =============================================================================

AWalkerSpawnerBase::AWalkerSpawnerBase(const FObjectInitializer& ObjectInitializer) :
  Super(ObjectInitializer),
  RandomStream(Seed)
{
  PrimaryActorTick.bCanEverTick = true;
  PrimaryActorTick.TickGroup = TG_PrePhysics;
}

// =============================================================================
// -- Overriden from AActor ----------------------------------------------------
// =============================================================================

void AWalkerSpawnerBase::BeginPlay()
{
  Super::BeginPlay();

  NumberOfWalkers = FMath::Max(0, NumberOfWalkers);

  // Allocate space for walkers.
  Walkers.Reserve(NumberOfWalkers);

  // Set seed for random numbers.
  if (!bUseFixedSeed) {
    RandomStream.GenerateNewSeed();
  } else {
    RandomStream.Initialize(Seed);
  }

  // Find spawn points present in level.
  TArray<AWalkerSpawnPointBase *> BeginSpawnPoints;
  for (TActorIterator<AWalkerSpawnPointBase> It(GetWorld()); It; ++It) {
    BeginSpawnPoints.Add(*It);
    AWalkerSpawnPoint *SpawnPoint = Cast<AWalkerSpawnPoint>(*It);
    if (SpawnPoint != nullptr) {
      SpawnPoints.Add(SpawnPoint);
    }
  }
  UE_LOG(LogCarla, Log, TEXT("Found %d positions for spawning walkers at begin play."), BeginSpawnPoints.Num());
  UE_LOG(LogCarla, Log, TEXT("Found %d positions for spawning walkers during game play."), SpawnPoints.Num());

  if (SpawnPoints.Num() < 2) {
    bSpawnWalkers = false;
    UE_LOG(LogCarla, Error, TEXT("We don't have enough spawn points for walkers!"));
  } else if (BeginSpawnPoints.Num() < NumberOfWalkers) {
    UE_LOG(LogCarla, Warning, TEXT("Requested %d walkers, but we only have %d spawn points. Some will fail to spawn."), NumberOfWalkers, BeginSpawnPoints.Num());
  }

  if (bSpawnWalkers) {
    uint32 Count = 0u;
    for (auto i = 0; i < NumberOfWalkers; ++i) {
      if (TryToSpawnWalkerAt(*BeginSpawnPoints[i % BeginSpawnPoints.Num()])) {
        ++Count;
      }
    }
    UE_LOG(LogCarla, Log, TEXT("Spawned %d walkers at begin play."), Count);
  }
}

void AWalkerSpawnerBase::Tick(float DeltaTime)
{
  Super::Tick(DeltaTime);

  if (bSpawnWalkers && (NumberOfWalkers > GetCurrentNumberOfWalkers())) {
    // Try to spawn one walker.
    TryToSpawnWalkerAt(GetRandomSpawnPoint());
  }

  if (WalkersBlackList.Num() > 0) {
    // If still stuck in the black list, just kill it.
    const int32 Index = (++CurrentIndexToCheck % WalkersBlackList.Num());
    auto Walker = WalkersBlackList[Index];
    auto Controller = GetController(Walker);
    if ((Controller == nullptr) ||
        (Controller->WalkerIsStuck())) {
      WalkersBlackList.RemoveAtSwap(Index);
      if (Walker != nullptr) {
        Walker->Destroy();
      }
    }
  }

  if (Walkers.Num() > 0) {
    // Check one walker, if fails black-list it or kill it.
    const int32 Index = (++CurrentIndexToCheck % Walkers.Num());
    auto Walker = Walkers[Index];
    auto Controller = GetController(Walker);
    if (Controller == nullptr) {
      Walkers.RemoveAtSwap(Index);
      if (Walker != nullptr) {
        Walker->Destroy();
      }
    } else if (Controller->WalkerIsStuck()) {
      TrySetDestination(*Walker);
      WalkersBlackList.Add(Walker);
      Walkers.RemoveAtSwap(Index);
    }
  }
}

// =============================================================================
// -- Other member functions ---------------------------------------------------
// =============================================================================

void AWalkerSpawnerBase::SetNumberOfWalkers(const int32 Count)
{
  if (Count > 0) {
    bSpawnWalkers = true;
    NumberOfWalkers = Count;
  } else {
    bSpawnWalkers = false;
  }
}

const AWalkerSpawnPointBase &AWalkerSpawnerBase::GetRandomSpawnPoint() const
{
  check(SpawnPoints.Num() > 0);
  return *SpawnPoints[RandomStream.RandRange(0, SpawnPoints.Num() - 1)];
}

bool AWalkerSpawnerBase::TryGetValidDestination(const FVector &Origin, FVector &Destination) const
{
  const auto &DestinationPoint = GetRandomSpawnPoint();
  Destination = DestinationPoint.GetActorLocation();
  return (GetDistance(Origin, Destination) >= MinimumWalkDistance);
}

bool AWalkerSpawnerBase::TryToSpawnWalkerAt(const AWalkerSpawnPointBase &SpawnPoint)
{
  // Try find destination.
  FVector Destination;
  if (!TryGetValidDestination(SpawnPoint.GetActorLocation(), Destination)) {
    return false;
  }

  // Spawn walker.
  ACharacter *Walker;
  SpawnWalker(SpawnPoint.GetActorTransform(), Walker);
  if (!WalkerIsValid(Walker)) {
    return false;
  }

  // Assign controller.
  Walker->AIControllerClass = AWalkerAIController::StaticClass();
  Walker->SpawnDefaultController();
  auto Controller = GetController(Walker);
  if (Controller == nullptr) { // Sometimes fails...
    UE_LOG(LogCarla, Error, TEXT("Something went wrong creating the controller for the new walker"));
    Walker->Destroy();
    return false;
  }

  // Add walker and set destination.
  Walkers.Add(Walker);
  Controller->MoveToLocation(Destination);
  return true;
}

bool AWalkerSpawnerBase::TrySetDestination(ACharacter &Walker) const
{
  // Try to retrieve controller.
  auto Controller = GetController(&Walker);
  if (Controller == nullptr) {
    return false;
  }

  // Try find destination.
  FVector Destination;
  if (!TryGetValidDestination(Walker.GetActorLocation(), Destination)) {
    return false;
  }

  Controller->MoveToLocation(Destination);
  return true;
}
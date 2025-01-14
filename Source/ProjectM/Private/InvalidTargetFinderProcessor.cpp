// Fill out your copyright notice in the Description page of Project Settings.


#include "InvalidTargetFinderProcessor.h"
#include "MassEnemyTargetFinderProcessor.h"
#include "MassTrackTargetProcessor.h"
#include "MassCommonFragments.h"
#include "MassMoveToCommandProcessor.h"
#include "MassSignalSubsystem.h"
#include "MassStateTreeTypes.h"
#include "MassEntityView.h"
#include "MassProjectileDamageProcessor.h"
#include "MassTargetFinderSubsystem.h"
#include <MassNavMeshMoveProcessor.h>

void UnstashMoveTarget(const FMassMoveTargetFragment& Source, FMassMoveTargetFragment& Destination, const UWorld& World, const FMassExecutionContext& Context, FMassNavMeshMoveFragment& NavMeshMoveFragment, const FTransform& EntityTransform)
{
	const bool bIsInNavMeshMove = Context.DoesArchetypeHaveTag<FMassNeedsNavMeshMoveTag>();
	// If entity is in a nav mesh move, add actions to get to the stashed move target instead of just copying since it might be impossible to get to directly.
	if (bIsInNavMeshMove)
	{
		const FVector& EntityLocation = EntityTransform.GetLocation();
		TArray<FNavigationAction>& Actions = NavMeshMoveFragment.ActionList.Get()->Actions;
		const FVector& ForwardToNewMoveTarget = (Source.Center - EntityLocation).GetSafeNormal();
		Actions.Insert(FNavigationAction(EntityLocation, ForwardToNewMoveTarget, EMassMovementAction::Stand), NavMeshMoveFragment.CurrentActionIndex);
		Actions.Insert(FNavigationAction(Source.Center, ForwardToNewMoveTarget, EMassMovementAction::Move), NavMeshMoveFragment.CurrentActionIndex + 1);
		Actions.Insert(FNavigationAction(Source.Center, Source.Forward, EMassMovementAction::Stand), NavMeshMoveFragment.CurrentActionIndex + 2);
		NavMeshMoveFragment.ActionsRemaining += 3;

		Destination.CreateNewAction(EMassMovementAction::Stand, World);
		Destination.Center = EntityLocation;
		Destination.Forward = ForwardToNewMoveTarget;
		Destination.DistanceToGoal = 0.f;
		Destination.bOffBoundaries = true;
		Destination.DesiredSpeed.Set(0.f);
		Destination.IntentAtGoal = EMassMovementAction::Move;
	}
	else
	{
		CopyMoveTarget(Source, Destination, World);
	}
}

void CopyMoveTarget(const FMassMoveTargetFragment& Source, FMassMoveTargetFragment& Destination, const UWorld& World)
{
	Destination.CreateNewAction(Source.GetCurrentAction(), World);
	Destination.bOffBoundaries = Source.bOffBoundaries;
	Destination.Center = Source.Center;
	Destination.Forward = Source.Forward;
	Destination.DistanceToGoal = Source.DistanceToGoal;
	Destination.DesiredSpeed = Source.DesiredSpeed;
	Destination.SlackRadius = Source.SlackRadius;
	Destination.bOffBoundaries = Source.bOffBoundaries;
	Destination.bSteeringFallingBehind = Source.bSteeringFallingBehind;
	Destination.IntentAtGoal = Source.IntentAtGoal;
}

UInvalidTargetFinderProcessor::UInvalidTargetFinderProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::All);
}

void UInvalidTargetFinderProcessor::Initialize(UObject& Owner)
{
	SignalSubsystem = UWorld::GetSubsystem<UMassSignalSubsystem>(Owner.GetWorld());
	TargetFinderSubsystem = UWorld::GetSubsystem<UMassTargetFinderSubsystem>(Owner.GetWorld());
}

void UInvalidTargetFinderProcessor::ConfigureQueries()
{
	BuildQueueEntityQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	BuildQueueEntityQuery.AddRequirement<FTargetEntityFragment>(EMassFragmentAccess::ReadWrite);
	BuildQueueEntityQuery.AddRequirement<FTeamMemberFragment>(EMassFragmentAccess::ReadOnly);
	BuildQueueEntityQuery.AddTagRequirement<FMassWillNeedEnemyTargetTag>(EMassFragmentPresence::All);

	BuildQueueForTrackTargetEntityQuery.AddRequirement<FTargetEntityFragment>(EMassFragmentAccess::ReadWrite);
	BuildQueueForTrackTargetEntityQuery.AddTagRequirement<FMassTrackTargetTag>(EMassFragmentPresence::All);

	InvalidateTargetsEntityQuery.AddRequirement<FTargetEntityFragment>(EMassFragmentAccess::ReadWrite);
	InvalidateTargetsEntityQuery.AddRequirement<FMassStashedMoveTargetFragment>(EMassFragmentAccess::ReadOnly, EMassFragmentPresence::Optional);
	InvalidateTargetsEntityQuery.AddRequirement<FMassMoveTargetFragment>(EMassFragmentAccess::ReadWrite, EMassFragmentPresence::Optional);
	InvalidateTargetsEntityQuery.AddRequirement<FMassNavMeshMoveFragment>(EMassFragmentAccess::ReadWrite);
	InvalidateTargetsEntityQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
}

bool IsTargetEntityOutOfRange(const FVector& TargetEntityLocation, const FVector &EntityLocation, const UMassEntitySubsystem& EntitySubsystem, const FMassEntityHandle Entity, const bool bIsEntitySoldier)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.IsTargetEntityOutOfRange);

	const double& DistanceBetweenEntities = (TargetEntityLocation - EntityLocation).Size();

	const float MaxRange = GetEntityRange(bIsEntitySoldier);

#if WITH_MASSGAMEPLAY_DEBUG
	if (UE::Mass::Debug::IsDebuggingEntity(Entity))
	{
		AsyncTask(ENamedThreads::GameThread, [World = EntitySubsystem.GetWorld(), EntityLocation, TargetEntityLocation]()
		{
			DrawDebugDirectionalArrow(World, EntityLocation, TargetEntityLocation, 10.f, FColor::Yellow, false, 0.1f);
		});
	}
#endif

	return DistanceBetweenEntities > MaxRange;
}

bool DidCapsulesCollide(const FCapsule& Capsule1, const FCapsule& Capsule2, const FMassEntityHandle& Entity, const UWorld& World)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DidCapsulesCollide);
	return TestCapsuleCapsule(Capsule1, Capsule2);
}

bool IsTargetEntityObstructed(const FVector& EntityLocation, const FVector& TargetEntityLocation, const UMassTargetFinderSubsystem& TargetFinderSubsystem, const FMassEntityHandle& Entity, const UMassEntitySubsystem& EntitySubsystem, const bool& IsEntityOnTeam1, const bool bIsEntitySoldier, const float TargetMinCaliberForDamage, const FMassEntityView& TargetEntityView, const FTransform& EntityTransform)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.IsTargetEntityObstructed);

	const FVector Buffer(10.f, 10.f, 10.f); // We keep a buffer in case EntityLocation and TargetEntityLocation are same value on any axis.
	FBox QueryBounds(EntityLocation.ComponentMin(TargetEntityLocation) - Buffer, EntityLocation.ComponentMax(TargetEntityLocation) + Buffer);
	TArray<FMassTargetGridItem> CloseEntities;
	TargetFinderSubsystem.GetTargetGrid().Query(QueryBounds, CloseEntities);

#if WITH_MASSGAMEPLAY_DEBUG
	if (UE::Mass::Debug::IsDebuggingEntity(Entity))
	{
		AsyncTask(ENamedThreads::GameThread, [QueryBounds, World = EntitySubsystem.GetWorld()]()
		{
			const FVector QueryCenter = (QueryBounds.Min + QueryBounds.Max) / 2.f;
			const FVector VerticalOffset(0.f, 0.f, 1000.f);
			DrawDebugBox(World, QueryCenter, QueryBounds.Max - QueryCenter + VerticalOffset, FColor::Blue, false, 0.1f);
		});
	}
#endif

	const bool& bIsTargetEntitySoldier = TargetEntityView.HasTag<FMassProjectileDamagableSoldierTag>();
	const FCapsule& ProjectileTraceCapsule = GetProjectileTraceCapsuleToTarget(bIsEntitySoldier, bIsTargetEntitySoldier, EntityTransform, TargetEntityLocation);

	for (const FMassTargetGridItem& OtherEntity : CloseEntities)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.IsTargetEntityObstructed.ProcessCloseEntity);

		// Skip self.
		if (OtherEntity.Entity == Entity)
		{
			continue;
		}

		// Skip invalid entities.
		if (!EntitySubsystem.IsEntityValid(OtherEntity.Entity))
		{
			continue;
		}

		// If same team or undamageable, check for collision.
		if (IsEntityOnTeam1 == OtherEntity.bIsOnTeam1 || !CanEntityDamageTargetEntity(TargetMinCaliberForDamage, OtherEntity.MinCaliberForDamage)) {
			const FCapsule& OtherEntityCapsule = TargetFinderSubsystem.GetTargetDynamicData()[OtherEntity.Entity].Capsule;
			if (DidCapsulesCollide(ProjectileTraceCapsule, OtherEntityCapsule, Entity, *EntitySubsystem.GetWorld()))
			{
				return true;
			}
		}
	}

	bool bIsTargetEntityVisibleViaSphereTrace;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.IsTargetEntityObstructed.IsTargetEntityVisibleViaSphereTrace);
		bIsTargetEntityVisibleViaSphereTrace = IsTargetEntityVisibleViaSphereTrace(*EntitySubsystem.GetWorld(), ProjectileTraceCapsule.a, ProjectileTraceCapsule.b, false);
	}
	return !bIsTargetEntityVisibleViaSphereTrace;
}

bool UInvalidTargetFinderProcessor_ShouldInvalidateAllTargets = false;

static void InvalidateAllTargets()
{
	UInvalidTargetFinderProcessor_ShouldInvalidateAllTargets = true;
}

static FAutoConsoleCommand InvalidateAllTargetsCmd(
	TEXT("pm.InvalidateAllTargets"),
	TEXT("InvalidateAllTargets"),
	FConsoleCommandDelegate::CreateStatic(InvalidateAllTargets)
);

bool IsTargetValid(const FMassEntityHandle& Entity, const FMassEntityHandle& TargetEntity, const UMassEntitySubsystem& EntitySubsystem, const float TargetMinCaliberForDamage, const UMassTargetFinderSubsystem& TargetFinderSubsystem, const bool& IsEntityOnTeam1, const bool bIsEntitySoldier, const FTransform& EntityTransform, const bool bInvalidateAllTargets, const bool bOnlyCheckIfTargetEntityValidInEntitySubsystem)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.IsTargetValid);

	const FVector& EntityLocation = EntityTransform.GetLocation();

	if (bInvalidateAllTargets)
	{
		return false;
	}

	if (!EntitySubsystem.IsEntityValid(TargetEntity))
	{
		return false;
	}

	if (bOnlyCheckIfTargetEntityValidInEntitySubsystem)
	{
		return true;
	}

	const FMassEntityView TargetEntityView(EntitySubsystem, TargetEntity);
	const FVector& TargetEntityLocation = TargetEntityView.GetFragmentData<FTransformFragment>().GetTransform().GetLocation();
	if (IsTargetEntityOutOfRange(TargetEntityLocation, EntityLocation, EntitySubsystem, Entity, bIsEntitySoldier))
	{
		return false;
	}

	if (IsTargetEntityObstructed(EntityLocation, TargetEntityLocation, TargetFinderSubsystem, Entity, EntitySubsystem, IsEntityOnTeam1, bIsEntitySoldier, TargetMinCaliberForDamage, TargetEntityView, EntityTransform))
	{
		return false;
	}

	return true;
}

struct FProcessEntityData
{
	FMassEntityHandle Entity;
	FMassEntityHandle TargetEntity;
	float TargetMinCaliberForDamage;
	FTransform EntityTransform;
	bool bIsEntityOnTeam1;
	bool bIsEntitySoldier;
	bool bOnlyCheckIfTargetEntityValidInEntitySubsystem = false;
};

/** Returns true if entity has invalid target. */
bool ProcessEntity(const FProcessEntityData& ProcessEntityData, const bool bInvalidateAllTargets, const UMassEntitySubsystem& EntitySubsystem, const UMassTargetFinderSubsystem& TargetFinderSubsystem, TQueue<FMassEntityHandle, EQueueMode::Mpsc>& EntitiesWithInvalidTargetQueue)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.ProcessEntity);

	const FMassEntityHandle& TargetEntity = ProcessEntityData.TargetEntity;
	if (!IsTargetValid(ProcessEntityData.Entity, TargetEntity, EntitySubsystem, ProcessEntityData.TargetMinCaliberForDamage, TargetFinderSubsystem, ProcessEntityData.bIsEntityOnTeam1, ProcessEntityData.bIsEntitySoldier, ProcessEntityData.EntityTransform, bInvalidateAllTargets, ProcessEntityData.bOnlyCheckIfTargetEntityValidInEntitySubsystem))
	{
		EntitiesWithInvalidTargetQueue.Enqueue(ProcessEntityData.Entity);
		return true;
	}
	return false;
}

void UInvalidTargetFinderProcessor::Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.Execute);

	if (!TargetFinderSubsystem)
	{
		return;
	}

	TQueue<FProcessEntityData, EQueueMode::Mpsc> EntitiesToCheckQueue;
	std::atomic<int32> TotalNumEntities = 0;

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.Execute.BuildQueue);

		BuildQueueEntityQuery.ParallelForEachEntityChunk(EntitySubsystem, Context, [&EntitiesToCheckQueue, &TotalNumEntities](FMassExecutionContext& Context)
		{
			const int32 NumEntities = Context.GetNumEntities();

			const TConstArrayView<FTransformFragment> TransformList = Context.GetFragmentView<FTransformFragment>();
			const TArrayView<FTargetEntityFragment> TargetEntityList = Context.GetMutableFragmentView<FTargetEntityFragment>();
			const TConstArrayView<FTeamMemberFragment> TeamMemberList = Context.GetFragmentView<FTeamMemberFragment>();

			for (int32 EntityIndex = 0; EntityIndex < NumEntities; ++EntityIndex)
			{
				FProcessEntityData ProcessEntityData;
				ProcessEntityData.Entity = Context.GetEntity(EntityIndex);
				ProcessEntityData.TargetEntity = TargetEntityList[EntityIndex].Entity;
				ProcessEntityData.TargetMinCaliberForDamage = TargetEntityList[EntityIndex].TargetMinCaliberForDamage;
				ProcessEntityData.EntityTransform = TransformList[EntityIndex].GetTransform();
				ProcessEntityData.bIsEntityOnTeam1 = TeamMemberList[EntityIndex].IsOnTeam1;
				ProcessEntityData.bIsEntitySoldier = Context.DoesArchetypeHaveTag<FMassProjectileDamagableSoldierTag>();
				EntitiesToCheckQueue.Enqueue(ProcessEntityData);
				TotalNumEntities++;
			}
		});
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.Execute.BuildQueueForTrackTarget);

		BuildQueueForTrackTargetEntityQuery.ParallelForEachEntityChunk(EntitySubsystem, Context, [&EntitiesToCheckQueue, &TotalNumEntities](FMassExecutionContext& Context)
		{
			const int32 NumEntities = Context.GetNumEntities();

			const TArrayView<FTargetEntityFragment> TargetEntityList = Context.GetMutableFragmentView<FTargetEntityFragment>();

			for (int32 EntityIndex = 0; EntityIndex < NumEntities; ++EntityIndex)
			{
				FProcessEntityData ProcessEntityData;
				ProcessEntityData.Entity = Context.GetEntity(EntityIndex);
				ProcessEntityData.TargetEntity = TargetEntityList[EntityIndex].Entity;
				ProcessEntityData.bOnlyCheckIfTargetEntityValidInEntitySubsystem = true;
				EntitiesToCheckQueue.Enqueue(ProcessEntityData);
				TotalNumEntities++;
			}
		});
	}

	TArray<FProcessEntityData> EntitiesToCheck;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.Execute.ConvertQueueToArray);

		EntitiesToCheck.Reserve(TotalNumEntities);
		while (!EntitiesToCheckQueue.IsEmpty())
		{
			FProcessEntityData ProcessEntityData;
			const bool bSuccess = EntitiesToCheckQueue.Dequeue(ProcessEntityData);
			check(bSuccess);
			EntitiesToCheck.Add(ProcessEntityData);
		}
	}

	TQueue<FMassEntityHandle, EQueueMode::Mpsc> EntitiesWithInvalidTargetQueue;
	std::atomic<int32> NumEntitiesWithInvalidTarget = 0;

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.Execute.ProcessEntities);

		const bool bInvalidateAllTargets = UInvalidTargetFinderProcessor_ShouldInvalidateAllTargets;

		ParallelFor(EntitiesToCheck.Num(), [&](const int32 JobIndex)
		{
			if (ProcessEntity(EntitiesToCheck[JobIndex], bInvalidateAllTargets, EntitySubsystem, *TargetFinderSubsystem.Get(), EntitiesWithInvalidTargetQueue))
			{
				NumEntitiesWithInvalidTarget++;
			}
		});
	}

	TSet<FMassEntityHandle> EntitiesWithInvalidTargets;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.Execute.BuildInvalidTargetsSet);

		EntitiesWithInvalidTargets.Reserve(NumEntitiesWithInvalidTarget);
		while (!EntitiesWithInvalidTargetQueue.IsEmpty())
		{
			FMassEntityHandle Entity;
			const bool bSuccess = EntitiesWithInvalidTargetQueue.Dequeue(Entity);
			check(bSuccess);
			EntitiesWithInvalidTargets.Add(Entity);
		}
	}

	TQueue<FMassEntityHandle, EQueueMode::Mpsc> EntitiesWithUnstashedMoveTargetQueue;

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.Execute.ProcessInvalidTargets);

		const UWorld& World = *EntitySubsystem.GetWorld();
		InvalidateTargetsEntityQuery.ParallelForEachEntityChunk(EntitySubsystem, Context, [&World, &EntitiesWithInvalidTargets, &EntitiesWithUnstashedMoveTargetQueue](FMassExecutionContext& Context)
		{
			const int32 NumEntities = Context.GetNumEntities();

			const TArrayView<FTargetEntityFragment> TargetEntityList = Context.GetMutableFragmentView<FTargetEntityFragment>();
			const TConstArrayView<FMassStashedMoveTargetFragment> StashedMoveTargetList = Context.GetFragmentView<FMassStashedMoveTargetFragment>();
			const TArrayView<FMassMoveTargetFragment> MoveTargetList = Context.GetMutableFragmentView<FMassMoveTargetFragment>();
			const TArrayView<FMassNavMeshMoveFragment> NavMeshMoveList = Context.GetMutableFragmentView<FMassNavMeshMoveFragment>();
			const TConstArrayView<FTransformFragment> TransformList = Context.GetFragmentView<FTransformFragment>();

			for (int32 EntityIndex = 0; EntityIndex < NumEntities; ++EntityIndex)
			{
				const FMassEntityHandle& Entity = Context.GetEntity(EntityIndex);
				const bool bIsInvalid = EntitiesWithInvalidTargets.Contains(Entity);
				if (bIsInvalid)
				{
					TargetEntityList[EntityIndex].Entity.Reset();
					const FMassStashedMoveTargetFragment* StashedMoveTargetFragment = StashedMoveTargetList.Num() > 0 ? &StashedMoveTargetList[EntityIndex] : nullptr;
					FMassMoveTargetFragment* MoveTargetFragment = MoveTargetList.Num() > 0 ? &MoveTargetList[EntityIndex] : nullptr;
					if (Context.DoesArchetypeHaveTag<FMassHasStashedMoveTargetTag>() && StashedMoveTargetFragment && MoveTargetFragment)
					{
						UnstashMoveTarget(*StashedMoveTargetFragment, *MoveTargetFragment, World, Context, NavMeshMoveList[EntityIndex], TransformList[EntityIndex].GetTransform());
						EntitiesWithUnstashedMoveTargetQueue.Enqueue(Entity);
					}
				}
			}
		});
	}


	UInvalidTargetFinderProcessor_ShouldInvalidateAllTargets = false;

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UInvalidTargetFinderProcessor.Execute.ProcessQueues);

		TransientEntitiesToSignal.Reset();

		for (const FMassEntityHandle& Entity : EntitiesWithInvalidTargets)
		{
			Context.Defer().AddTag<FMassNeedsEnemyTargetTag>(Entity);
			Context.Defer().RemoveTag<FMassWillNeedEnemyTargetTag>(Entity);
			Context.Defer().RemoveTag<FMassTrackTargetTag>(Entity);
			TransientEntitiesToSignal.Add(Entity);
		}

		while (!EntitiesWithUnstashedMoveTargetQueue.IsEmpty())
		{
			FMassEntityHandle Entity;
			const bool bSuccess = EntitiesWithUnstashedMoveTargetQueue.Dequeue(Entity);
			check(bSuccess);

			Context.Defer().RemoveTag<FMassHasStashedMoveTargetTag>(Entity);
		}

		if (TransientEntitiesToSignal.Num())
		{
			SignalSubsystem->SignalEntities(UE::Mass::Signals::NewStateTreeTaskRequired, TransientEntitiesToSignal);
		}
	}
}

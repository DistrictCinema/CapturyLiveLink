// Copyright The Captury GmbH 2025

#include "CapturyLiveLinkSource.h"
#include "ILiveLinkClient.h"
#include "RemoteCaptury.h"

#undef SetPort

#include "Async/AsyncWork.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "GenericPlatform/GenericPlatformMath.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "InterpolationProcessor/LiveLinkBasicFrameInterpolateProcessor.h"
#include "LiveLinkAnimationVirtualSubject.h"
#include "LiveLinkVirtualSubject.h"
#include "Misc/EngineVersionComparison.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Roles/LiveLinkTransformRole.h"
#include "Roles/LiveLinkTransformTypes.h"
#include <cmath>

#define DEG2RADf 0.0174532925199432958f

#define LOCTEXT_NAMESPACE "Captury"

#define scaleToUnreal 0.1f // unreal works in cm

int CapturyLiveLinkSource::sourceCount = 0;
TMap<FString, TSet<int>> CapturyLiveLinkSource::ipAddressCounts;

DECLARE_LOG_CATEGORY_EXTERN(LogCaptury, Log, All);
DEFINE_LOG_CATEGORY(LogCaptury);

static void actorChanged(RemoteCaptury* rc, int actorId, int mode, void* userArg)
{
	((CapturyLiveLinkSource*)userArg)->actorChanged(actorId, mode);
}

void CapturyLiveLinkSource::actorChanged(int actorId, int mode)
{
	const CapturyActor* actor = Captury_getActor(remoteCaptury, actorId);
	UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink:%s actor %x changed to mode %s"), (actor == nullptr) ? TEXT(" unknown") : TEXT(""), actorId, ANSI_TO_TCHAR(CapturyActorStatusString[mode]));
	Captury_freeActor(remoteCaptury, actor);
	{
		FScopeLock guard(&mutx); lockedAt = __LINE__; unlockedAt = -1;
		if (haveActors.Contains(actorId)) {
			if (mode == ACTOR_STOPPED || mode == ACTOR_DELETED) {
				// The lock used in RemoveSubject_AnyThread is called on
				// LiveLinkClient::Tick which calls CapturyLiveLinkSource::Update function causing deadlock when an actor is changed at the same time a tick is
				Captury_log(remoteCaptury, CAPTURY_LOG_INFO, "Unreal: actor %x now has mode %s. deleting.", actorId, CapturyActorStatusString[mode]);
				queuedActorIdsToRemove.Enqueue(actorId);
				unlockedAt = __LINE__;
				return;
			}
			Captury_log(remoteCaptury, CAPTURY_LOG_WARNING, "Unreal: actor %x now has mode %s. already have actor.", actorId, CapturyActorStatusString[mode]);
			UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: already have actor %x"), actorId);
			unlockedAt = __LINE__;
			return;
		}
	} unlockedAt = __LINE__;

	if (mode == ACTOR_STOPPED || mode == ACTOR_DELETED) {
		Captury_log(remoteCaptury, CAPTURY_LOG_WARNING, "Unreal: actor %x now has mode %s. already gone.", actorId, CapturyActorStatusString[mode]);
		UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: cannot stop actor %x. already gone."), actorId);
		return;
	}

	Captury_log(remoteCaptury, CAPTURY_LOG_INFO, "Unreal: actor %x now has mode %s. adding.", actorId, CapturyActorStatusString[mode]);
	UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: pushing new actor %x"), actorId);
	mutx.Lock(); lockedAt = __LINE__; unlockedAt = -1;
	queuedActorIds.Enqueue(actorId);
	mutx.Unlock(); unlockedAt = __LINE__;
}

static void newPose(RemoteCaptury* rc, CapturyActor* actor, CapturyPose* pose, int trackingQuality, void* userArg)
{
	((CapturyLiveLinkSource*)userArg)->newPose(actor, pose, trackingQuality);
}

void CapturyLiveLinkSource::newPose(CapturyActor* actor, CapturyPose* pose, int trackingQuality)
{
	// static uint64_t lastT = 0;
	// if (pose->timestamp / 1000000 != lastT) {
	// 	lastT = pose->timestamp / 1000000;
	// 	Captury_log(CAPTURY_LOG_INFO, "PluginInterface: streaming at %zd nc", pose->timestamp);
	// 	for (int i = 0; i < pose->numTransforms; ++i) {
	// 		Captury_log(CAPTURY_LOG_INFO, "  %d: %.2f %.2f %.2f   %.2f %.2f %.2f", i,
	// 			pose->transforms[i].rotation[0], pose->transforms[i].rotation[1], pose->transforms[i].rotation[2],
	// 			pose->transforms[i].translation[0], pose->transforms[i].translation[1], pose->transforms[i].translation[2]);
	// 	}
	// }

	mutx.Lock(); lockedAt = __LINE__; unlockedAt = -1;
	if (liveLinkClient == nullptr) {
		mutx.Unlock(); unlockedAt = __LINE__;
		return;
	}
	const FLiveLinkSubjectKey* subjectKey = haveActors.Find(actor->id);
	if (subjectKey == nullptr) {
		queuedActorIds.Enqueue(actor->id); // lock enqueue access because it is only thread safe for single producer - single consumer workflows and we have two producers here
		UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: pushing new actor %x %s"), actor->id, ANSI_TO_TCHAR(actor->name));
		mutx.Unlock(); unlockedAt = __LINE__;
		return;
	}

	mutx.Unlock(); unlockedAt = __LINE__;

	//

	FLiveLinkFrameDataStruct animFrameData(FLiveLinkAnimationFrameData::StaticStruct());
	FLiveLinkAnimationFrameData& animData = *animFrameData.Cast<FLiveLinkAnimationFrameData>();
	FLiveLinkFrameDataStruct trafoFrameData(FLiveLinkTransformFrameData::StaticStruct());
	FLiveLinkTransformFrameData& trafoData = *trafoFrameData.Cast<FLiveLinkTransformFrameData>();
	if (!framerate.IsValid()) {
		int num, denom;
		Captury_getFramerate(remoteCaptury, &num, &denom);
		framerate = FFrameRate(num, denom);
	}

	// we could transform Captury's time into local time here but that's complicated and the question is what anyone would need it for
	if (actor->numJoints > 1) {
		animData.WorldTime = FPlatformTime::Seconds();
		animData.MetaData.SceneTime = FQualifiedFrameTime(framerate.AsFrameTime(pose->timestamp * 1e-6), framerate);

		// raw timestamp as reported by CapturyLive (converted to seconds)
		animData.MetaData.StringMetaData.Add(FName(TEXT("TimestampInSeconds")), FString::Printf(TEXT("%f"), pose->timestamp * 1e-6));
		// tracking / streaming frame rate
		animData.MetaData.StringMetaData.Add(FName(TEXT("FrameRate")), FString::Printf(TEXT("%f"), framerate.Numerator / (double)framerate.Denominator));
		animData.MetaData.StringMetaData.Add(FName(TEXT("FrameNumber")), FString::Printf(TEXT("%d"), animData.MetaData.SceneTime.Time.FrameNumber.Value));

		for (int i = 0; i < actor->numMetaData; ++i)
			animData.MetaData.StringMetaData.Add(FName(actor->metaDataKeys[i]), actor->metaDataValues[i]);
	} else {
		trafoData.WorldTime = FPlatformTime::Seconds();
		trafoData.MetaData.SceneTime = FQualifiedFrameTime(framerate.AsFrameTime(pose->timestamp * 1e-6), framerate);

		// raw timestamp as reported by CapturyLive (converted to seconds)
		trafoData.MetaData.StringMetaData.Add(FName(TEXT("TimestampInSeconds")), FString::Printf(TEXT("%f"), pose->timestamp * 1e-6));
		// tracking / streaming frame rate
		trafoData.MetaData.StringMetaData.Add(FName(TEXT("FrameRate")), FString::Printf(TEXT("%f"), framerate.Numerator / (double)framerate.Denominator));
		trafoData.MetaData.StringMetaData.Add(FName(TEXT("FrameNumber")), FString::Printf(TEXT("%d"), trafoData.MetaData.SceneTime.Time.FrameNumber.Value));

		for (int i = 0; i < actor->numMetaData; ++i)
			trafoData.MetaData.StringMetaData.Add(FName(actor->metaDataKeys[i]), actor->metaDataValues[i]);
	}

	TArray<FQuat> globalOrientations;
	TArray<FQuat> globalPoseRotations;
	TArray<float> globalScale;

	static int once = 0;

	// add Root joint
	if (actor->numJoints > 1 && strcmp(actor->joints[0].name, "Hips") == 0)
		animData.Transforms.Add(FTransform(FQuat(0.0f, 0.0f, 0.0f, 1.0f), FVector::ZeroVector, FVector::OneVector));

	FQuat rot;
	FVector trans;
	for (int i = 0; i < pose->numTransforms; ++i) {
		float rx = pose->transforms[i].rotation[0] * DEG2RADf;
		float ry = pose->transforms[i].rotation[1] * DEG2RADf;
		float rz = pose->transforms[i].rotation[2] * DEG2RADf;
		FQuat poseRot = FQuat(FVector(0, 0, 1), rz) * FQuat(FVector(0, 1, 0), ry) * FQuat(FVector(1, 0, 0), rx);
		//poseRot.X = pose->transforms[i].rotation[0];
		//poseRot.Y = pose->transforms[i].rotation[1];
		//poseRot.Z = pose->transforms[i].rotation[2];
		//poseRot.W = 1.0f - poseRot.X * poseRot.X - poseRot.Y * poseRot.Y - poseRot.Z * poseRot.Z;
		//poseRot.W = (poseRot.W <= 0.0f) ? 0.0f : std::sqrt(poseRot.W);
		globalPoseRotations.Add(poseRot);

		if (actor->joints[i].parent >= 0) { // make local rotation
			if (actor->joints[i].parent >= globalPoseRotations.Num()) {
				UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: actor %s: parent of joint %d is invalid (%d)"), ANSI_TO_TCHAR(actor->name), i, actor->joints[i].parent);
				return;
			}
			poseRot = globalPoseRotations[actor->joints[i].parent].Inverse() * poseRot;
		}

		FQuat bindPose;
		bindPose.X = actor->joints[i].orientation[0];
		bindPose.Y = actor->joints[i].orientation[1];
		bindPose.Z = actor->joints[i].orientation[2];
		bindPose.W = 1.0f - bindPose.X * bindPose.X - bindPose.Y * bindPose.Y - bindPose.Z * bindPose.Z;
		bindPose.W = (bindPose.W <= 0.0f) ? 0.0f : std::sqrt(bindPose.W);
		globalOrientations.Add(bindPose);

		if (once < 3) {
			if (i != 0) {
				FQuat q(globalOrientations[actor->joints[i].parent].Inverse() * bindPose);
				q.Y = -q.Y;
				q.W = -q.W;
				FRotator r(q);
				UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: actor %s joint %s rel: %g %g %g (%g, %g, %g, %g)"), ANSI_TO_TCHAR(actor->name), ANSI_TO_TCHAR(actor->joints[i].name), r.Roll, r.Pitch, r.Yaw, bindPose.W, bindPose.X, bindPose.Y, bindPose.Z);
			} else {
				FRotator r(bindPose);
				UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: actor %s joint %s: %g %g %g (%g, %g, %g, %g)"), ANSI_TO_TCHAR(actor->name), ANSI_TO_TCHAR(actor->joints[i].name), r.Roll, r.Pitch, r.Yaw, bindPose.W, bindPose.X, bindPose.Y, bindPose.Z);
			}
		}

		float parentScale;
		if (i == 0) {
			parentScale = 1.0f;
			trans = FVector(pose->transforms[i].translation[0] * scaleToUnreal,
					pose->transforms[i].translation[1] * scaleToUnreal,
					pose->transforms[i].translation[2] * scaleToUnreal);

			// rotate Y-is-up to Z-is-up
			rot = FQuat(FVector(1, 0, 0), 90 * DEG2RADf) * poseRot * bindPose;
			trans = FQuat(FVector(1, 0, 0), 90 * DEG2RADf) * trans;
		} else {
			parentScale = globalScale[actor->joints[i].parent];
			trans = FVector(actor->joints[i].offset[0] * scaleToUnreal / parentScale,
					actor->joints[i].offset[1] * scaleToUnreal / parentScale,
					actor->joints[i].offset[2] * scaleToUnreal / parentScale);

			// relative to parent
			FQuat relBindPose = globalOrientations[actor->joints[i].parent].Inverse() * bindPose;
			trans = globalOrientations[actor->joints[i].parent].Inverse() * trans;
			rot = relBindPose * globalOrientations[i].Inverse() * poseRot * globalOrientations[i];
		}
		float scale = actor->joints[i].scale[0];
		globalScale.Add(parentScale * scale);

		// unreal does this during FBX loading for some reason
		rot.Y = -rot.Y;
		rot.W = -rot.W;

		// switch from right-handed to left-handed coordinate system
		trans.Y = -trans.Y;

		FTransform trafo(rot, trans, FVector(scale));
		if (actor->numJoints > 1)
			animData.Transforms.Add(trafo);
		else
			trafoData.Transform = trafo;
	}

	// add blend shapes as properties
	if (pose->numBlendShapes != 0) {
		TArray<float>& propVals = animFrameData.GetBaseData()->PropertyValues;
		propVals.SetNumZeroed(pose->numBlendShapes);
		for (int i = 0; i < pose->numBlendShapes; ++i)
			propVals[i] = pose->blendShapeActivations[i];
	}

	once++;

	if (actor->numJoints > 1)
		liveLinkClient->PushSubjectFrameData_AnyThread(*subjectKey, MoveTemp(animFrameData));
	else
		liveLinkClient->PushSubjectFrameData_AnyThread(*subjectKey, MoveTemp(trafoFrameData));
}

static void arTagDetected(RemoteCaptury* remoteCaptury, int num, CapturyARTag* tags, void* userArg)
{
	((CapturyLiveLinkSource*)userArg)->arTagDetected(num, tags);
}

void CapturyLiveLinkSource::arTagDetected(int num, CapturyARTag* tags)
{
	if (liveLinkClient == nullptr)
		return;

	for (int i = 0; i < num; ++i) {
		mutx.Lock(); lockedAt = __LINE__; unlockedAt = -1;
		const FLiveLinkSubjectKey* subjectKey = haveActors.Find(tags[i].id);
		if (subjectKey == nullptr) {
			mutx.Unlock(); unlockedAt = __LINE__;
			queuedARTags.Enqueue(tags[i].id);
			continue;
		}

		mutx.Unlock(); unlockedAt = __LINE__;

		float rx = tags[i].transform.rotation[0] * DEG2RADf;
		float ry = tags[i].transform.rotation[1] * DEG2RADf;
		float rz = tags[i].transform.rotation[2] * DEG2RADf;
		FQuat poseRot = FQuat(FVector(0, 0, 1), rz) * FQuat(FVector(0, 1, 0), ry) * FQuat(FVector(1, 0, 0), rx);

		FVector trans = FVector(tags[i].transform.translation[0] * scaleToUnreal,
					tags[i].transform.translation[1] * scaleToUnreal,
					tags[i].transform.translation[2] * scaleToUnreal);

		// rotate Y-is-up to Z-is-up
		FQuat rot = FQuat(FVector(1, 0, 0), 90 * DEG2RADf) * poseRot;
		trans = FQuat(FVector(1, 0, 0), 90 * DEG2RADf) * trans;

		// unreal does this during FBX loading for some reason
		rot.Y = -rot.Y;
		rot.W = -rot.W;

		// switch from right-handed to left-handed coordinate system
		trans.Y = -trans.Y;

		FTransform trafo(rot, trans, FVector::OneVector);

		FLiveLinkFrameDataStruct frameData(FLiveLinkTransformFrameData::StaticStruct());
		FLiveLinkTransformFrameData& data = *frameData.Cast<FLiveLinkTransformFrameData>();
		data.WorldTime = FPlatformTime::Seconds();

		data.Transform = trafo;

		liveLinkClient->PushSubjectFrameData_AnyThread(*subjectKey, MoveTemp(frameData));
	}
}

CapturyLiveLinkSource::CapturyLiveLinkSource(const FText& ip, bool useTCP, bool streamARTags, bool streamCompressed) : ipAddress(ip), enabled(true), status(LOCTEXT("statusConnecting", "connecting")), connected(false), queuedActorIds(10), queuedActorIdsToRemove(10), queuedARTags(10)
{
	++sourceCount;
	sourceIndex = 1;
	if (ipAddressCounts.Contains(ip.ToString())) {
		TSet<int>& indexes = ipAddressCounts[ip.ToString()];
		while (indexes.Contains(sourceIndex))
			++sourceIndex;
		indexes.Add(sourceIndex);
	} else
		ipAddressCounts.Add(ip.ToString(), {1});

	if (sourceCount > 1) {
		if (sourceIndex == 1)
			prefix = FString::Printf(TEXT("%s:"), *ip.ToString());
		else
			prefix = FString::Printf(TEXT("%s{%d}:"), *ip.ToString(), sourceIndex);
	}

	UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: connecting to %s, tcp: %d, artags: %d, compressed: %d, idx: %d, prefix %s"), *ip.ToString(), useTCP, streamARTags, streamCompressed, sourceIndex, *prefix);

	remoteCaptury = Captury_create();
	if (remoteCaptury) {
		Captury_enablePrintf(remoteCaptury, 0);
		Captury_connect2(remoteCaptury, TCHAR_TO_ANSI(*ip.ToString()), 2101, 0, 0, 1);
		int numerator, denominator;
		Captury_getFramerate(remoteCaptury, &numerator, &denominator);
		framerate = FFrameRate(numerator, denominator);

		Captury_registerNewPoseCallback(remoteCaptury, ::newPose, this);
		Captury_registerActorChangedCallback(remoteCaptury, ::actorChanged, this);
		Captury_registerARTagCallback(remoteCaptury, ::arTagDetected, this);

		int what = CAPTURY_STREAM_GLOBAL_POSES | CAPTURY_STREAM_BLENDSHAPES | CAPTURY_STREAM_ONLY_ROOT_TRANSLATION;
		if (useTCP)
			what |= CAPTURY_STREAM_TCP;
		if (streamARTags)
			what |= CAPTURY_STREAM_ARTAGS;
		if (streamCompressed)
			what |= CAPTURY_STREAM_COMPRESSED;
		Captury_startStreaming(remoteCaptury, what);
	}
}

FLiveLinkStaticDataStruct CapturyLiveLinkSource::setupPropStaticData()
{
	FLiveLinkStaticDataStruct staticData;
	staticData.InitializeWith(FLiveLinkTransformStaticData::StaticStruct(), nullptr);
	FLiveLinkTransformStaticData* data = staticData.Cast<FLiveLinkTransformStaticData>();
	check(data);
	data->bIsScaleSupported = false;

	return staticData;
}

FLiveLinkStaticDataStruct CapturyLiveLinkSource::setupSkeletonDefinition(const CapturyActor* actor)
{
	FLiveLinkStaticDataStruct staticData;
	staticData.InitializeWith(FLiveLinkSkeletonStaticData::StaticStruct(), nullptr);
	FLiveLinkSkeletonStaticData* skelData = staticData.Cast<FLiveLinkSkeletonStaticData>();
	check(skelData);

	TArray<FName> jointNames;
	TArray<int32> parents;
	int jointOffset = 0;
	// add Root joint
	if (strcmp(actor->joints[0].name, "Hips") == 0) {
		FString name("Root");
		jointNames.Emplace(name);
		parents.Emplace(-1);
		jointOffset = 1;
	}
	// add other joints
	for (int32 i = 0; i < actor->numJoints; ++i) {
		FString name(ANSI_TO_TCHAR(actor->joints[i].name));
		name.ReplaceInline(TEXT("."), TEXT("_"));
		jointNames.Emplace(name);
		parents.Emplace(actor->joints[i].parent + jointOffset);
	}
	skelData->SetBoneNames(jointNames);
	skelData->SetBoneParents(parents);

	TArray<FName> blendShapeNames;
	for (int32 i = 0; i < actor->numBlendShapes; ++i) {
		FString name(ANSI_TO_TCHAR(actor->blendShapes[i].name));
		name.ReplaceInline(TEXT("."), TEXT("_"));
		blendShapeNames.Emplace(name);
	}
	skelData->PropertyNames = blendShapeNames;

	return staticData;
}

void CapturyLiveLinkSource::addSubject(const CapturyActor* actor) // mutx is held here
{
	if (haveActors.Find(actor->id) != 0) {
		UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: already have actor %x %s"), actor->id, ANSI_TO_TCHAR(actor->name));
		return;
	}

	if (liveLinkClient == nullptr)
		return;

	UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink update: created subject %x %s"), actor->id, ANSI_TO_TCHAR(actor->name));

	FName name(FString::Printf(TEXT("%s%s"), *prefix, ANSI_TO_TCHAR(actor->name)));
	FLiveLinkSubjectKey subjectKey(sourceGuid, name);

	if (actor->numJoints > 1) {
		FLiveLinkStaticDataStruct skeletonDefinition = CapturyLiveLinkSource::setupSkeletonDefinition(actor);
		liveLinkClient->PushSubjectStaticData_AnyThread(subjectKey, ULiveLinkAnimationRole::StaticClass(), MoveTemp(skeletonDefinition));
	} else {
		FLiveLinkStaticDataStruct transformDefinition = CapturyLiveLinkSource::setupPropStaticData();
		liveLinkClient->PushSubjectStaticData_AnyThread(subjectKey, ULiveLinkTransformRole::StaticClass(), MoveTemp(transformDefinition));
	}

	haveActors.FindOrAdd(actor->id, subjectKey);
}

void CapturyLiveLinkSource::addSubjects()
{
	check(IsInGameThread());

	const CapturyActor* actors = nullptr;
	int numActors = Captury_getActors(remoteCaptury, &actors);
	UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: got %d actors"), numActors);

	mutx.Lock(); lockedAt = __LINE__; unlockedAt = -1;
	for (int i = 0; i < numActors; ++i)
		addSubject(&actors[i]);
	mutx.Unlock(); unlockedAt = __LINE__;

	Captury_freeActors(remoteCaptury);
}

void CapturyLiveLinkSource::Update()
{
	int actorId;
	mutx.Lock(); lockedAt = __LINE__; unlockedAt = -1;
	TArray<int> requeue;

	if (liveLinkClient == nullptr)
		return;

	while (queuedActorIdsToRemove.Dequeue(actorId)) {
		FScopeLock guard(&mutx); lockedAt = __LINE__; unlockedAt = -1;
		const FLiveLinkSubjectKey* subjectKey = haveActors.Find(actorId);
		if (subjectKey == nullptr)
			continue;

		liveLinkClient->RemoveSubject_AnyThread(*subjectKey); // The lock used on AnyThread is used on LiveLinkClient::Tick which calls this function causing deadlock if called by another thread

		haveActors.Remove(actorId);
		haveActors.Compact();
		UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: removing stopped actor %x"), actorId);
		unlockedAt = __LINE__;
	}

	while (queuedActorIds.Dequeue(actorId)) {
		const CapturyActor* actor = Captury_getActor(remoteCaptury, actorId);
		if (actor != nullptr) {
			addSubject(actor);
			Captury_freeActor(remoteCaptury, actor);
		} else
			requeue.Push(actorId);
	}

	for (int id : requeue) {
		UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink update: requeue %x"), id);
		queuedActorIds.Enqueue(id);
	}
	mutx.Unlock(); unlockedAt = __LINE__;

	int id;
	while (queuedARTags.Dequeue(id)) {
		FName name(FString::Printf(TEXT("%sARTag %d"), *prefix, id));
		FLiveLinkSubjectKey subjectKey = FLiveLinkSubjectKey(sourceGuid, name);
		FLiveLinkStaticDataStruct skeletonDefinition = CapturyLiveLinkSource::setupPropStaticData();
		liveLinkClient->PushSubjectStaticData_AnyThread(subjectKey, ULiveLinkTransformRole::StaticClass(), MoveTemp(skeletonDefinition));

		mutx.Lock(); lockedAt = __LINE__; unlockedAt = -1;
		haveActors.Add(id, subjectKey);
		mutx.Unlock(); unlockedAt = __LINE__;
	}
}

bool CapturyLiveLinkSource::IsSourceStillValid() const
{
	bool stillValid = (enabled && Captury_getConnectionStatus(remoteCaptury) == CAPTURY_CONNECTED);
	if (!stillValid) {
		status = LOCTEXT("statusFailed", "failed to connect");
		return false;
	}

	return true;
}

CapturyLiveLinkSource::~CapturyLiveLinkSource()
{
	Captury_destroy(remoteCaptury);

	--sourceCount;
	ipAddressCounts[ipAddress.ToString()].Remove(sourceIndex);
}

FText CapturyLiveLinkSource::GetSourceType() const
{
	return LOCTEXT("sourcetype", "Captury Live Link");
}

void CapturyLiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
	sourceGuid = InSourceGuid;
	liveLinkClient = InClient;

	UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: receive client %s"), *liveLinkClient->ModularFeatureName.ToString());

	addSubjects();
}

void CapturyLiveLinkSource::disable()
{
	UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: disabling"));
	status = LOCTEXT("statusDisabled", "disabled");
	Captury_startStreaming(remoteCaptury, CAPTURY_STREAM_NOTHING);

	mutx.Lock(); lockedAt = __LINE__; unlockedAt = -1;
	haveActors.Reset();

	liveLinkClient = nullptr;
	mutx.Unlock(); unlockedAt = __LINE__;

	enabled = false;
}

void CapturyLiveLinkSource::setIPAddress(const FText& ip)
{
	ipAddress = ip;
	Captury_connect(remoteCaptury, TCHAR_TO_ANSI(*ip.ToString()), 2101);
}

bool CapturyLiveLinkSource::RequestSourceShutdown()
{
	UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: request shutdown"));
	Captury_stopStreaming(remoteCaptury, 0);

	mutx.Lock(); lockedAt = __LINE__; unlockedAt = -1;

	liveLinkClient = nullptr;
	mutx.Unlock(); unlockedAt = __LINE__;

	return true;
}

FText CapturyLiveLinkSource::GetSourceStatus() const
{
	switch (Captury_getConnectionStatus(remoteCaptury)) {
	case CAPTURY_DISCONNECTED:
		connected = false;
		return LOCTEXT("statusDisconnected", "connecting");
	case CAPTURY_CONNECTING:
		connected = false;
		return LOCTEXT("statusConnecting", "connecting...");
	case CAPTURY_CONNECTED:
		if (!connected) {
			const CapturyActor* actors = nullptr;
			int numActors = Captury_getActors(remoteCaptury, &actors);

			mutx.Lock(); lockedAt = __LINE__; unlockedAt = -1;
			for (int i = 0; i < numActors; ++i)
				queuedActorIds.Enqueue(actors[i].id);
			mutx.Unlock(); unlockedAt = __LINE__;
			UE_LOG(LogCaptury, Display, TEXT("CapturyLiveLink: status: connected with %d actors"), numActors);

			connected = true;
		}
		return LOCTEXT("statusConnected", "connected");
	}

	return LOCTEXT("statusUnknown", "unknown");
}

#undef LOCTEXT_NAMESPACE

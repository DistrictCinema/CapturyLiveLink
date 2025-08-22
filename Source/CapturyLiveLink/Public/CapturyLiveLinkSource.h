// Copyright The Captury GmbH 2021

#pragma once

#include "CoreMinimal.h"
#include "ILiveLinkSource.h"
#include "LiveLinkSubjectSettings.h"
#include "LiveLinkFrameInterpolationProcessor.h"
#include "LiveLinkFramePreProcessor.h"
#include "LiveLinkFrameTranslator.h"
#include "Containers/CircularQueue.h"

struct CapturyActor;
struct CapturyPose;
struct CapturyARTag;
struct CapturyCamera;
struct RemoteCaptury;

/**
 *
 */
class CAPTURYLIVELINK_API CapturyLiveLinkSource : public ILiveLinkSource
{
public:
	CapturyLiveLinkSource(const FText & ip, bool useTCP, bool streamARTags, bool streamCompressed);
	~CapturyLiveLinkSource();

	//	void setSource(TSharedPtr<ILiveLinkSource> src) { source = src; }

	void disable();
	void setIPAddress(const FText & ip);

	virtual void ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid);
	virtual void InitializeSettings(ULiveLinkSourceSettings* Settings) override {}

	// Can this source be displayed in the Source UI list
	virtual bool CanBeDisplayedInUI() const { return true; }

	virtual bool IsSourceStillValid() const override;

	virtual bool RequestSourceShutdown() override;

	virtual FText GetSourceType() const override;
	virtual FText GetSourceMachineName() const override {
		return ipAddress;
	}
	virtual FText GetSourceStatus() const override;

	virtual TSubclassOf< ULiveLinkSourceSettings > GetSettingsClass() const override { return nullptr; }
	static FLiveLinkStaticDataStruct setupPropStaticData();
	static FLiveLinkStaticDataStruct setupSkeletonDefinition(const CapturyActor* actor);
	void addSubjects();
	virtual void Update() override;
	virtual void OnSettingsChanged(ULiveLinkSourceSettings* Settings, const FPropertyChangedEvent& PropertyChangedEvent) override {}

	// public because they need to be called by static callbacks
	void actorChanged(int actorId, int mode);
	void newPose(CapturyActor* actor, CapturyPose* pose, int trackingQuality);
	void arTagDetected(int num, CapturyARTag* tags);
protected:
	void addSubject(const CapturyActor* actor);

	FText ipAddress;

	bool enabled;
	mutable FText status;
	mutable bool connected;

	RemoteCaptury* remoteCaptury = nullptr;

	ILiveLinkClient* liveLinkClient = nullptr;

	mutable int lockedAt;
	mutable int unlockedAt;
	mutable FCriticalSection mutx; // lock acccess to actors and cameras
	FGuid sourceGuid;

	TMap<int, FLiveLinkSubjectKey> haveActors;
	mutable TCircularQueue<int> queuedActorIds;
	TCircularQueue<int> queuedActorIdsToRemove;
	TCircularQueue<int> queuedARTags;
	FFrameRate framerate;

	// when there are multiple sources, add a prefix to the subject names
	FString prefix;
	// keep track of which source has which prefix on the same IP
	int sourceIndex;
	static int sourceCount;
	static TMap<FString, TSet<int>> ipAddressCounts;
};

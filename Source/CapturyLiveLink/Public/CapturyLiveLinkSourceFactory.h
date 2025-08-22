// Copyright The Captury GmbH 2021

#pragma once

#include "CoreMinimal.h"
#include "LiveLinkSourceFactory.h"
#include "CapturyLiveLinkSourceFactory.generated.h"

/**
 *
 */
UCLASS()
class CAPTURYLIVELINK_API UCapturyLiveLinkSourceFactory : public ULiveLinkSourceFactory
{
	GENERATED_BODY()

	UCapturyLiveLinkSourceFactory();

	~UCapturyLiveLinkSourceFactory();

public:
	virtual FText GetSourceDisplayName() const override;
	virtual FText GetSourceTooltip() const override;

	virtual EMenuType GetMenuType() const override { return EMenuType::SubPanel; }

	virtual TSharedPtr< SWidget > BuildCreationPanel(FOnLiveLinkSourceCreated OnLiveLinkSourceCreated) const override;
	virtual TSharedPtr< ILiveLinkSource > CreateSource(const FString & ConnectionString) const override;

private:
};

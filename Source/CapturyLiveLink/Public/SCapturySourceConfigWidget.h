// Copyright The Captury GmbH 2021

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "LiveLinkSourceFactory.h"
#include "IPAddress.h"

/**
 *
 */
class CAPTURYLIVELINK_API SCapturySourceConfigWidget : public SCompoundWidget, public FWidgetActiveTimerDelegate
{
public:
	SLATE_BEGIN_ARGS(SCapturySourceConfigWidget)
	{}
	SLATE_END_ARGS()

	/** Constructs this widget with InArgs */
	void Construct(const FArguments& InArgs);

	const FText& getIpAddress() const { return initialIP; }
	void setCallback(ULiveLinkSourceFactory::FOnLiveLinkSourceCreated whenCreated) { callback = whenCreated; }

	static TSharedPtr<ILiveLinkSource> createSource(const FString & ConnectionString);

protected:
	void useTCPChanged(ECheckBoxState newState);
	void streamARTagsChanged(ECheckBoxState newState);
	void streamCompressedChanged(ECheckBoxState newState);
	void openSource(const FText & InText, ETextCommit::Type type);
	FReply okClicked();

	static TWeakPtr<ILiveLinkSource> source; // there can be only one source
	ULiveLinkSourceFactory::FOnLiveLinkSourceCreated callback;

	UPROPERTY(EditAnywhere, Config, Category = Custom)
	static FText initialIP;
	static bool useTCP;
	static bool streamARTags;
	static bool streamCompressed;
};

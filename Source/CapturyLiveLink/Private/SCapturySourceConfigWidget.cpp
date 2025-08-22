// Copyright The Captury GmbH 2025

#include "SCapturySourceConfigWidget.h"
#include "SlateOptMacros.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"
#include "CapturyLiveLinkSourceFactory.h"
#include "CapturyLiveLinkSource.h"
#include "CoreGlobals.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersionComparison.h"

#undef SetPort
#include "SocketSubsystem.h"

#define LOCTEXT_NAMESPACE "Captury"

#define CAPTURY_LIVELINK_VERSION "v14 2025-06-17"

TWeakPtr<ILiveLinkSource> SCapturySourceConfigWidget::source;

FText SCapturySourceConfigWidget::initialIP = LOCTEXT("127.0.0.1", "127.0.0.1");
bool SCapturySourceConfigWidget::useTCP = false;
bool SCapturySourceConfigWidget::streamARTags = true;
bool SCapturySourceConfigWidget::streamCompressed = false;

BEGIN_SLATE_FUNCTION_BUILD_OPTIMIZATION
void SCapturySourceConfigWidget::Construct(const FArguments& InArgs)
{
	FString section = "CapturyLiveLink.SourceConfig";
	#if UE_VERSION_OLDER_THAN(5,1,0)
	GConfig->GetText(*section, TEXT("IP"), initialIP, GEditorSettingsIni);
	GConfig->GetBool(*section, TEXT("UseTCP"), useTCP, GEditorSettingsIni);
	GConfig->GetBool(*section, TEXT("StreamARTags"), streamARTags, GEditorSettingsIni);
	GConfig->GetBool(*section, TEXT("StreamCompressed"), streamCompressed, GEditorSettingsIni);
	#else
	initialIP =        GConfig->GetTextOrDefault(*section, TEXT("IP"), LOCTEXT("127.0.0.1", "127.0.0.1"), GEditorSettingsIni);
	useTCP =           GConfig->GetBoolOrDefault(*section, TEXT("UseTCP"), false, GEditorSettingsIni);
	streamARTags =     GConfig->GetBoolOrDefault(*section, TEXT("StreamARTags"), true, GEditorSettingsIni);
	streamCompressed = GConfig->GetBoolOrDefault(*section, TEXT("StreamCompressed"), false, GEditorSettingsIni);
	#endif

	ChildSlot.Padding(4,6,0,6)
	[
		// Populate the widget
		SNew(SGridPanel)
		+ SGridPanel::Slot(0, 0).Padding(4, 2)
		[
		    SNew(STextBlock).Text(LOCTEXT("Host", "Host:"))
		]
		+ SGridPanel::Slot(1, 0).Padding(4, 2)
		    [
			SNew(SEditableTextBox).Text(initialIP)
			.OnTextCommitted(this, &SCapturySourceConfigWidget::openSource)
		    ]
		+ SGridPanel::Slot(0, 1).Padding(4, 2)
		[
		    SNew(STextBlock).Text(LOCTEXT("UseTCP", "Use TCP:"))
		]
		+ SGridPanel::Slot(1, 1).Padding(4, 2)
		[
		    SNew(SCheckBox).IsChecked(useTCP)
		    .OnCheckStateChanged(this, &SCapturySourceConfigWidget::useTCPChanged)
		]
		+ SGridPanel::Slot(0, 2).Padding(4, 2)
		[
		    SNew(STextBlock).Text(LOCTEXT("StreamARTags", "Stream ARTags:"))
		]
		+ SGridPanel::Slot(1, 2).Padding(4, 2)
		[
		    SNew(SCheckBox).IsChecked(streamARTags)
		    .OnCheckStateChanged(this, &SCapturySourceConfigWidget::streamARTagsChanged)
		]
		+ SGridPanel::Slot(0, 3).Padding(4, 2)
		[
		    SNew(STextBlock).Text(LOCTEXT("StreamCompressed", "Stream Compressed:"))
		]
		+ SGridPanel::Slot(1, 3).Padding(4, 2)
		[
		    SNew(SCheckBox).IsChecked(streamCompressed)
		    .OnCheckStateChanged(this, &SCapturySourceConfigWidget::streamCompressedChanged)
		]
		+ SGridPanel::Slot(0, 4).Padding(4, 2).VAlign(VAlign_Center)
		[
		    SNew(STextBlock).Text(LOCTEXT("Version", CAPTURY_LIVELINK_VERSION))
		    .Font(FSlateFontInfo(FCoreStyle::GetDefaultFont(), 8))
		]
		+ SGridPanel::Slot(1, 4).Padding(4, 2)
		[
		    SNew(SButton).Text(LOCTEXT("OK", "Connect")).HAlign(HAlign_Center)
		    .OnClicked(this, &SCapturySourceConfigWidget::okClicked)
		]
	];
}
END_SLATE_FUNCTION_BUILD_OPTIMIZATION

FReply SCapturySourceConfigWidget::okClicked()
{
	openSource(initialIP, ETextCommit::OnEnter);

	return FReply::Handled();
}

void SCapturySourceConfigWidget::useTCPChanged(ECheckBoxState newState)
{
	useTCP = (newState == ECheckBoxState::Checked);
}

void SCapturySourceConfigWidget::streamARTagsChanged(ECheckBoxState newState)
{
	streamARTags = (newState == ECheckBoxState::Checked);
}

void SCapturySourceConfigWidget::streamCompressedChanged(ECheckBoxState newState)
{
	streamCompressed = (newState == ECheckBoxState::Checked);
}

void SCapturySourceConfigWidget::openSource(const FText & InText, ETextCommit::Type type)
{
	switch (type) {
	case ETextCommit::OnEnter: {
		FString connectionString = FString::Printf(TEXT("%s;%d;%d;%d"), *InText.ToString(), useTCP, streamARTags, streamCompressed);
		TSharedPtr<ILiveLinkSource> src = createSource(connectionString);
		callback.Execute(src, connectionString);
		initialIP = InText;
		break; }
	case ETextCommit::Default:
	case ETextCommit::OnUserMovedFocus:
		initialIP = InText;
		break;
	}
}

TSharedPtr<ILiveLinkSource> SCapturySourceConfigWidget::createSource(const FString & in)
{
	FText ip;

	TArray<FString> configs;
	in.ParseIntoArray(configs, TEXT(";"), true);
	FString input = configs[0];
	bool tcp = (configs.Num() >= 2) ? configs[1].Equals(TEXT("1")) : useTCP;
	bool artags = (configs.Num() >= 3) ? configs[2].Equals(TEXT("1")) : streamARTags;
	bool compressed = (configs.Num() >= 4) ? configs[3].Equals(TEXT("1")) : streamCompressed;

	FAddressInfoResult result = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetAddressInfo(*input, nullptr, EAddressInfoFlags::Default, NAME_None);
	if (result.ReturnCode == SE_NO_ERROR) {
		const TSharedRef<FInternetAddr>& addr = result.Results[0].Address;
		ip = FText::FromString(addr->ToString(false));
		UE_LOG(LogTemp, Display, TEXT("CapturyLiveLink: resolved host %s to %s"), *input, *ip.ToString());
	} else {
		UE_LOG(LogTemp, Warning, TEXT("CapturyLiveLink: cannot resolve host %s"), *input);
		return TSharedPtr<ILiveLinkSource>();
	}

	UE_LOG(LogTemp, Display, TEXT("CapturyLiveLink: create new source %s"), *in);

	CapturyLiveLinkSource* src = new CapturyLiveLinkSource(ip, tcp, artags, compressed);
	TSharedPtr<ILiveLinkSource> sharedPtr(src);
	source = sharedPtr;

	FString section = "CapturyLiveLink.SourceConfig";
	GConfig->SetText(*section, TEXT("IP"), ip, GEditorSettingsIni);
	GConfig->SetBool(*section, TEXT("UseTCP"), useTCP, GEditorSettingsIni);
	GConfig->SetBool(*section, TEXT("StreamARTags"), streamARTags, GEditorSettingsIni);
	GConfig->SetBool(*section, TEXT("StreamCompressed"), streamCompressed, GEditorSettingsIni);
	GConfig->Flush(false, GEditorSettingsIni);

	return sharedPtr;
}

#undef LOCTEXT_NAMESPACE

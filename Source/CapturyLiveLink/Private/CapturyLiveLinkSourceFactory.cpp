// Copyright The Captury GmbH 2021

#include "CapturyLiveLinkSourceFactory.h"
#include "SCapturySourceConfigWidget.h"

#define LOCTEXT_NAMESPACE "Captury"

UCapturyLiveLinkSourceFactory::UCapturyLiveLinkSourceFactory() {
	UE_LOG(LogTemp, Warning, TEXT("instantiating CapturyLiveLinkSourceFactory"));
}

UCapturyLiveLinkSourceFactory::~UCapturyLiveLinkSourceFactory() {
	UE_LOG(LogTemp, Warning, TEXT("destroying CapturyLiveLinkSourceFactory"));
}

FText UCapturyLiveLinkSourceFactory::GetSourceDisplayName() const
{
	return LOCTEXT("Captury Live", "Captury Live");
}

FText UCapturyLiveLinkSourceFactory::GetSourceTooltip() const
{
	return LOCTEXT("Connect to Captury Live", "Connect to Captury Live");
}

TSharedPtr<SWidget> UCapturyLiveLinkSourceFactory::BuildCreationPanel(FOnLiveLinkSourceCreated callback) const
{
	// UE_LOG(LogTemp, Warning, TEXT("CapturyLiveLinkSourceFactory::BuildCreationPanel"));
	auto wx = SNew(SCapturySourceConfigWidget);
	wx->setCallback(callback);
	TSharedPtr<SWidget> w = wx;// SNew(SCapturySourceConfigWidget);
	return w;
}

TSharedPtr< ILiveLinkSource > UCapturyLiveLinkSourceFactory::CreateSource(const FString & ConnectionString) const
{
	return SCapturySourceConfigWidget::createSource(ConnectionString);
}

#undef LOCTEXT_NAMESPACE

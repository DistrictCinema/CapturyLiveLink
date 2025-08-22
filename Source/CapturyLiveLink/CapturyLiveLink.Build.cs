// Copyright The Captury GmbH 2021

using UnrealBuildTool;

public class CapturyLiveLink : ModuleRules
{
	public CapturyLiveLink(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
		);


		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
		);


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"InputCore", // for slate
				"Engine",
				"Projects",
				"Sockets",
				"LiveLinkInterface",
				"LiveLink"
				// ... add other public dependencies that you statically link with here ...
			}
		);


		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// ... add private dependencies that you statically link with here ...
				"Slate",
				"SlateCore"
			    }
		);


		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
		);

		PublicDefinitions.Add("WINDOWS_IGNORE_PACKING_MISMATCH");
	}
}

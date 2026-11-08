// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;
using System.Collections.Generic;

public class MetaHumanTestTarget : TargetRules
{
	public MetaHumanTestTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7; // was V5: UBT refused to build the game with the installed 5.8 engine

		ExtraModuleNames.AddRange( new string[] { "MetaHumanTest" } );
	}
}

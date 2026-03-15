// Copyright Epic Games, Inc. All Rights Reserved.

#include "FramerManagerModule.h"
#include "FramerManager.h"

#define LOCTEXT_NAMESPACE "FFramerManagerModuleModule"

void FFramerManagerModuleModule::StartupModule()
{
	FFramerManager::OnStartup();
}

void FFramerManagerModuleModule::ShutdownModule()
{
	FFramerManager::OnShutdown();
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FFramerManagerModuleModule, FramerManagerModule)
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Brian Keating (EI6LF), Douglas J. Cerrato (KB2UKA), and contributors.
// See LICENSE at the repository root for the full text.
//
// sdk_includes.h — central VST 3 SDK include point.
//
// The SDK headers are not authored to the strict warning discipline our
// own translation units use (-Wall -Wextra -Werror, /W4 /WX). Including
// them through this header lets us bracket the SDK's includes with a
// pragma diagnostic push / pop pair and keep -Werror engaged everywhere
// else. Add new SDK includes here, not directly inside plugin_host.cpp,
// so all three platforms quarantine the warnings consistently.

#pragma once

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wunused-variable"
#  pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wuninitialized"
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#  if defined(__clang__)
#    pragma GCC diagnostic ignored "-Wunused-private-field"
#    pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#  endif
#elif defined(_MSC_VER)
#  pragma warning(push, 0)
#endif

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/uid.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/vsttypes.h"

#include "public.sdk/source/vst/utility/stringconvert.h"

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif
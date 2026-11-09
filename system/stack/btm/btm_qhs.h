/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */
#pragma once

#include <bluetooth/types/address.h>
#include <bluetooth/types/bt_transport.h>

#include <cstddef>
#include <cstdint>

void BTM_QhsInit();
void BTM_ConfigQHS();
bool BTM_IsQHSPhySupported(const RawAddress& bd_addr, tBT_TRANSPORT transport);
void BTM_GetQllLocalSupportedFeatures(uint8_t* features);
void BTM_QllVendorSpecificEvent(uint8_t subevent, const uint8_t* payload, size_t length);
void BTM_QhsTestMaybeInjectBrEdrState(const RawAddress& bd_addr);

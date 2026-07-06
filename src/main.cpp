// main.cpp : Component entry point
#include "stdafx.h"
// PCH ^

DECLARE_COMPONENT_VERSION(
    "WASAPI Exclusive Output",
    "2.0",
    "WASAPI exclusive-mode output support for foobar2000 v2.0 (64-bit).\n"
    "Provides bit-exact audio output by bypassing the Windows audio mixer.\n"
    "Supports push and event-driven output modes.\n"
    "\n"
    "Based on the foobar2000 SDK.\n"
);

VALIDATE_COMPONENT_FILENAME("foo_out_wasapi_x64.dll");

//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{

class WebAssembly
{
#ifdef ENABLE_WASM
public:
    // WebAssembly spec internal definition for page size
    static const uint PageSize = 64 * 1024;

    class EntryInfo
    {
    public:
        static FunctionInfo Compile;
        static FunctionInfo Validate;
    };
    static Var EntryCompile(RecyclableObject* function, CallInfo callInfo, ...);
    static Var EntryValidate(RecyclableObject* function, CallInfo callInfo, ...);

    static uint32 ToNonWrappingUint32(Var val, ScriptContext * ctx);
#endif
};

}

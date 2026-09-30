/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "src/gpu/ganesh/gl/builders/GrGLShaderStringBuilder.h"

#include "include/gpu/ganesh/gl/GrGLFunctions.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/private/base/SkTo.h"
#include "src/base/SkAutoMalloc.h"
#include "src/core/SkTraceEvent.h"
#include "src/gpu/ganesh/gl/GrGLDefines.h"
#include "src/gpu/ganesh/gl/GrGLGpu.h"
#include "src/gpu/ganesh/gl/GrGLUtil.h"
#include "src/core/SkChecksum.h"
#include "src/sksl/SkSLProgramSettings.h"
#include "src/sksl/SkSLString.h"
#include "src/sksl/SkSLUtil.h"

#include <cstdint>
#include <initializer_list>
#include <string>

uint64_t GrGLTranslationKey(const SkSL::ShaderCaps& caps, const SkSL::ProgramSettings& settings) {
    // Layout mirror of SkSL::ShaderCaps: a field added there must be added here and hashed below.
    struct KnownShaderCaps {
        SkSL::GLSLGeneration fGeneration;
        bool fFlags[39];
        const char* fStrings[5];
        SkSL::ShaderCaps::AdvBlendEqInteraction fAdvBlendEqInteraction;
    };
    static_assert(sizeof(KnownShaderCaps) == sizeof(SkSL::ShaderCaps));

    std::string bytes;
    auto flags = [&](std::initializer_list<bool> values) {
        for (bool value : values) {
            bytes.push_back(value ? '1' : '0');
        }
    };
    auto text = [&](const char* value) {
        bytes.push_back(value ? '"' : '-');
        bytes.append(value ? value : "");
        bytes.push_back('\0');
    };
    bytes.push_back(static_cast<char>(caps.fGLSLGeneration));
    bytes.push_back(static_cast<char>(caps.fAdvBlendEqInteraction));
    flags({caps.fDualSourceBlendingSupport, caps.fShaderDerivativeSupport,
           caps.fExplicitTextureLodSupport, caps.fIntegerSupport, caps.fNonsquareMatrixSupport,
           caps.fInverseHyperbolicSupport, caps.fFBFetchSupport, caps.fFBFetchNeedsCustomOutput,
           caps.fUsesPrecisionModifiers, caps.fFlatInterpolationSupport,
           caps.fNoPerspectiveInterpolationSupport, caps.fSampleMaskSupport,
           caps.fExternalTextureSupport, caps.fFloatIs32Bits, caps.fInfinitySupport,
           caps.fBuiltinFMASupport, caps.fBuiltinDeterminantSupport,
           caps.fCanUseVoidInSequenceExpressions, caps.fCanUseMinAndAbsTogether,
           caps.fCanUseFractForNegativeValues, caps.fMustForceNegatedAtanParamToFloat,
           caps.fMustForceNegatedLdexpParamToMultiply, caps.fAtan2ImplementedAsAtanYOverX,
           caps.fMustDoOpBetweenFloorAndAbs, caps.fMustGuardDivisionEvenAfterExplicitZeroCheck,
           caps.fCanUseFragCoord, caps.fAddAndTrueToLoopCondition,
           caps.fUnfoldShortCircuitAsTernary, caps.fEmulateAbsIntFunction,
           caps.fRewriteDoWhileLoops, caps.fRewriteSwitchStatements,
           caps.fRemovePowWithConstantExponent, caps.fNoDefaultPrecisionForExternalSamplers,
           caps.fRewriteMatrixVectorMultiply, caps.fRewriteMatrixComparisons,
           caps.fRemoveConstFromFunctionParameters, caps.fPerlinNoiseRoundingFix,
           caps.fMustDeclareFragmentFrontFacing, caps.fForceStd430ArrayLayout});
    text(caps.fVersionDeclString);
    text(caps.fShaderDerivativeExtensionString);
    text(caps.fExternalTextureExtensionString);
    text(caps.fSecondExternalTextureExtensionString);
    text(caps.fFBFetchColorName);
    // The settings Ganesh GL varies per program or context; the rest keep their defaults.
    flags({settings.fFragColorIsInOut, settings.fForceHighPrecision, settings.fSharpenTextures,
           settings.fForceNoRTFlip});
    return SkChecksum::Hash64(bytes.data(), bytes.size());
}

GrGLuint GrGLCompileAndAttachShader(const GrGLContext& glCtx,
                                    GrGLuint programId,
                                    GrGLenum type,
                                    const std::string& glsl,
                                    bool shaderWasCached,
                                    GrThreadSafePipelineBuilder::Stats* stats,
                                    GrContextOptions::ShaderErrorHandler* errorHandler,
                                    bool deferStatus) {
    TRACE_EVENT0_ALWAYS("skia.shaders", "driver_compile_shader");
    const GrGLInterface* gli = glCtx.glInterface();

    // Specify GLSL source to the driver.
    GrGLuint shaderId;
    GR_GL_CALL_RET(gli, shaderId, CreateShader(type));
    if (0 == shaderId) {
        return 0;
    }
    const GrGLchar* source = glsl.c_str();
    GrGLint sourceLength = SkToInt(glsl.size());
    GR_GL_CALL(gli, ShaderSource(shaderId, 1, &source, &sourceLength));

    stats->incShaderCompilations();
    GR_GL_CALL(gli, CompileShader(shaderId));

    if (!deferStatus) {
        ATRACE_ANDROID_FRAMEWORK("checkCompiled");
        GrGLint compiled = GR_GL_INIT_ZERO;
        GR_GL_CALL(gli, GetShaderiv(shaderId, GR_GL_COMPILE_STATUS, &compiled));

        if (!compiled) {
            GrGLint infoLen = GR_GL_INIT_ZERO;
            GR_GL_CALL(gli, GetShaderiv(shaderId, GR_GL_INFO_LOG_LENGTH, &infoLen));
            SkAutoMalloc log(sizeof(char)*(infoLen+1)); // outside if for debugger
            if (infoLen > 0) {
                // retrieve length even though we don't need it to workaround bug in Chromium cmd
                // buffer param validation.
                GrGLsizei length = GR_GL_INIT_ZERO;
                GR_GL_CALL(gli, GetShaderInfoLog(shaderId, infoLen+1, &length, (char*)log.get()));
            }
            errorHandler->compileError(
                    glsl.c_str(), infoLen > 0 ? (const char*)log.get() : "", shaderWasCached);
            GR_GL_CALL(gli, DeleteShader(shaderId));
            return 0;
        }
    }

    // Attach the shader, but defer deletion until after we have linked the program.
    // This works around a bug in the Android emulator's GLES2 wrapper which
    // will immediately delete the shader object and free its memory even though it's
    // attached to a program, which then causes glLinkProgram to fail.
    GR_GL_CALL(gli, AttachShader(programId, shaderId));
    return shaderId;
}

bool GrGLCheckLinkStatus(const GrGLGpu* gpu,
                         GrGLuint programID,
                         bool shaderWasCached,
                         GrContextOptions::ShaderErrorHandler* errorHandler,
                         const std::string* sksl[kGrShaderTypeCount],
                         const std::string glsl[kGrShaderTypeCount]) {
    const GrGLInterface* gli = gpu->glInterface();

    GrGLint linked = GR_GL_INIT_ZERO;
    GR_GL_CALL(gli, GetProgramiv(programID, GR_GL_LINK_STATUS, &linked));
    if (!linked && errorHandler) {
        std::string allShaders;
#if defined(SK_DEBUG)
        #define SKSL_FORMAT "// Vertex SKSL\n%s\n// Fragment SKSL\n%s\n"
#else
        #define SKSL_FORMAT "%s\n%s\n"
#endif
        if (sksl) {
            SkSL::String::appendf(&allShaders, SKSL_FORMAT,
                                               sksl[kVertex_GrShaderType]->c_str(),
                                               sksl[kFragment_GrShaderType]->c_str());
        }
        if (glsl) {
            SkSL::String::appendf(&allShaders, SKSL_FORMAT,
                                               glsl[kVertex_GrShaderType].c_str(),
                                               glsl[kFragment_GrShaderType].c_str());
        }
#undef SKSL_FORMAT
        GrGLint infoLen = GR_GL_INIT_ZERO;
        GR_GL_CALL(gli, GetProgramiv(programID, GR_GL_INFO_LOG_LENGTH, &infoLen));
        SkAutoMalloc log(infoLen+1);
        if (infoLen > 0) {
            // retrieve length even though we don't need it to workaround
            // bug in chrome cmd buffer param validation.
            GrGLsizei length = GR_GL_INIT_ZERO;
            GR_GL_CALL(gli, GetProgramInfoLog(programID, infoLen+1, &length, (char*)log.get()));
        }
        const char* errorMsg = (infoLen > 0) ? (const char*)log.get()
                                             : "link failed but did not provide an info log";
        errorHandler->compileError(allShaders.c_str(), errorMsg, shaderWasCached);
    }
    return SkToBool(linked);
}

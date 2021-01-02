/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "BlurFilter.h"
#include "BlurNoise.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <ui/GraphicTypes.h>
#include <cstdint>

#include <utils/Trace.h>

// Minimum and maximum sampling offsets for each pass count, determined empirically.
// Too low: bilinear downsampling artifacts
// Too high: diagonal sampling artifacts
static const std::vector<std::tuple<float, float>> kOffsetRanges = {
    {1.00,  2.50}, // pass 1
    {1.25,  4.25}, // pass 2
    {1.50, 11.25}, // pass 3
    {1.75, 18.00}, // pass 4
    {2.00, 20.00}, // pass 5
    /* limited by kMaxPasses */
};

namespace android {
namespace renderengine {
namespace gl {

BlurFilter::BlurFilter(GLESRenderEngine& engine)
      : mEngine(engine),
        mCompositionFbo(engine),
        mDitherFbo(engine),
        mMixProgram(engine),
        mColorSpaceProgram(engine),
        mDownsampleProgram(engine),
        mUpsampleProgram(engine) {
    // Create VBO first for usage in shader VAOs
    static constexpr auto size = 2.0f;
    static constexpr auto translation = 1.0f;
    // This represents the rectangular display with a single oversized triangle.
    const GLfloat vboData[] = {
        // Vertex data
        translation - size, -translation - size,
        translation - size, -translation + size,
        translation + size, -translation + size,
        // UV data
        0.0f, 0.0f - translation,
        0.0f, size - translation,
        size, size - translation
    };
    mMeshBuffer.allocateBuffers(vboData, 12 /* size */);

    mMixProgram.compile(getVertexShader(), getMixFragShader());
    mMPosLoc = mMixProgram.getAttributeLocation("aPosition");
    mMUvLoc = mMixProgram.getAttributeLocation("aUV");
    mMCompositionTextureLoc = mMixProgram.getUniformLocation("uCompositionTexture");
    mMBlurredTextureLoc = mMixProgram.getUniformLocation("uBlurredTexture");
    mMDitherTextureLoc = mMixProgram.getUniformLocation("uDitherTexture");
    mMBlurOpacityLoc = mMixProgram.getUniformLocation("uBlurOpacity");
    createVertexArray(&mMVertexArray, mMPosLoc, mMUvLoc);

    mColorSpaceProgram.compile(getVertexShader(), getColorSpaceFragShader());
    mCPosLoc = mColorSpaceProgram.getAttributeLocation("aPosition");
    mCUvLoc = mColorSpaceProgram.getAttributeLocation("aUV");
    mCTextureLoc = mColorSpaceProgram.getUniformLocation("uTexture");
    createVertexArray(&mCVertexArray, mCPosLoc, mCUvLoc);

    mDownsampleProgram.compile(getVertexShader(), getDownsampleFragShader());
    mDPosLoc = mDownsampleProgram.getAttributeLocation("aPosition");
    mDUvLoc = mDownsampleProgram.getAttributeLocation("aUV");
    mDTextureLoc = mDownsampleProgram.getUniformLocation("uTexture");
    mDOffsetLoc = mDownsampleProgram.getUniformLocation("uOffset");
    mDHalfPixelLoc = mDownsampleProgram.getUniformLocation("uHalfPixel");
    createVertexArray(&mDVertexArray, mDPosLoc, mDUvLoc);

    mUpsampleProgram.compile(getVertexShader(), getUpsampleFragShader());
    mUPosLoc = mUpsampleProgram.getAttributeLocation("aPosition");
    mUUvLoc = mUpsampleProgram.getAttributeLocation("aUV");
    mUTextureLoc = mUpsampleProgram.getUniformLocation("uTexture");
    mUOffsetLoc = mUpsampleProgram.getUniformLocation("uOffset");
    mUHalfPixelLoc = mUpsampleProgram.getUniformLocation("uHalfPixel");
    createVertexArray(&mUVertexArray, mUPosLoc, mUUvLoc);

    mDitherFbo.allocateBuffers(64, 64, (void *) kBlurNoisePattern,
                               GL_NEAREST, GL_REPEAT,
                               GL_RGB, GL_RGB, GL_UNSIGNED_BYTE);
}

status_t BlurFilter::prepareBuffers(const DisplaySettings& display) {
    ATRACE_NAME("BlurFilter::prepareBuffers");

    // Source FBO, used for blurring and crossfading at full resolution
    mDisplayWidth = display.physicalDisplay.width();
    mDisplayHeight = display.physicalDisplay.height();
    mCompositionFbo.allocateBuffers(mDisplayWidth, mDisplayHeight);
    if (mCompositionFbo.getStatus() != GL_FRAMEBUFFER_COMPLETE) {
        ALOGE("Invalid composition buffer");
        return mCompositionFbo.getStatus();
    }

    // Only allocate FBO wrapper objects once
    if (mPassFbos.size() == 0) {
        mPassFbos.reserve(kMaxPasses + 1);
        for (auto i = 0; i < kMaxPasses + 1; i++) {
            mPassFbos.push_back(new GLFramebuffer(mEngine));
        }
    }

    // Allocate FBOs for blur passes, using downscaled display size
    const uint32_t sourceFboWidth = floorf(mDisplayWidth * kFboScale);
    const uint32_t sourceFboHeight = floorf(mDisplayHeight * kFboScale);
    for (auto i = 0; i < kMaxPasses + 1; i++) {
        GLFramebuffer* fbo = mPassFbos[i];

        fbo->allocateBuffers(sourceFboWidth >> i, sourceFboHeight >> i, nullptr,
                                GL_LINEAR, GL_CLAMP_TO_EDGE,
                                // 2-10-10-10 reversed is the only 10-bpc format in GLES 3.1
                                GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT);
        if (fbo->getStatus() != GL_FRAMEBUFFER_COMPLETE) {
            ALOGE("Invalid pass buffer");
            return fbo->getStatus();
        }
    }

    // Creating a BlurFilter doesn't necessarily mean that it will be used, so we
    // only check for successful shader compiles here.
    if (!mMixProgram.isValid()) {
        ALOGE("Invalid mix shader");
        return GL_INVALID_OPERATION;
    }
    if (!mDownsampleProgram.isValid()) {
        ALOGE("Invalid downsample shader");
        return GL_INVALID_OPERATION;
    }
    if (!mUpsampleProgram.isValid()) {
        ALOGE("Invalid upsample shader");
        return GL_INVALID_OPERATION;
    }

    return NO_ERROR;
}

status_t BlurFilter::setAsDrawTarget(const DisplaySettings& display, uint32_t radius) {
    ATRACE_NAME("BlurFilter::setAsDrawTarget");

    mDisplayX = display.physicalDisplay.left;
    mDisplayY = display.physicalDisplay.top;

    // Allocating FBOs is expensive, so only reallocate for larger displays.
    // Smaller displays will still work using oversized buffers.
    if (mDisplayWidth < display.physicalDisplay.width() ||
            mDisplayHeight < display.physicalDisplay.height()) {
        status_t status = prepareBuffers(display);
        if (status != NO_ERROR) {
            return status;
        }
    }

    // Approximate Gaussian blur radius
    if (radius != mRadius) {
        mRadius = radius;
        auto [passes, offset] = convertGaussianRadius(radius);
        ALOGI("SARU: ---------------------------------- new radius: %d  : passes=%d offset=%f", radius, passes, offset);
        mPasses = passes;
        mOffset = offset;
    }

    mCompositionFbo.bind();
    glViewport(0, 0, mCompositionFbo.getBufferWidth(), mCompositionFbo.getBufferHeight());
    return NO_ERROR;
}

std::tuple<int32_t, float> BlurFilter::convertGaussianRadius(uint32_t radius) {
    // Test each pass level first
    for (auto i = 0; i < kMaxPasses; i++) {
        auto [minOffset, maxOffset] = kOffsetRanges[i];
        float offset = radius * kFboScale / std::pow(2, i + 1);
        if (offset >= minOffset && offset <= maxOffset) {
            return {i + 1, offset};
        }
    }

    // FIXME: handle minmax properly
    return {1, radius * kFboScale / std::pow(2, 1)};
}

void BlurFilter::createVertexArray(GLuint* vertexArray, GLuint position, GLuint uv) {
    glGenVertexArrays(1, vertexArray);
    glBindVertexArray(*vertexArray);
    mMeshBuffer.bind();

    glEnableVertexAttribArray(position);
    glVertexAttribPointer(position, 2 /* size */, GL_FLOAT, GL_FALSE,
                          2 * sizeof(GLfloat) /* stride */, 0 /* offset */);

    glEnableVertexAttribArray(uv);
    glVertexAttribPointer(uv, 2 /* size */, GL_FLOAT, GL_FALSE, 0 /* stride */,
                          (GLvoid*)(6 * sizeof(GLfloat)) /* offset */);

    mMeshBuffer.unbind();
    glBindVertexArray(0);
}

void BlurFilter::drawMesh(GLuint vertexArray) {
    glBindVertexArray(vertexArray);
    glDrawArrays(GL_TRIANGLES, 0 /* first */, 3 /* vertices */);
    glBindVertexArray(0);
}

void BlurFilter::renderPass(GLFramebuffer* read, GLFramebuffer* draw, GLuint halfPixelLoc, GLuint vertexArray) {
    auto targetWidth = draw->getBufferWidth();
    auto targetHeight = draw->getBufferHeight();
    glViewport(0, 0, targetWidth, targetHeight);

    ALOGI("SARU: blur to %dx%d", targetWidth, targetHeight);

    glBindTexture(GL_TEXTURE_2D, read->getTextureName());
    draw->bind();

    // 1/2 pixel offset in texture coordinate (UV) space
    // Note that this is different from NDC!
    glUniform2f(halfPixelLoc, 0.5 / targetWidth, 0.5 / targetHeight);
    drawMesh(vertexArray);
}

status_t BlurFilter::prepare() {
    ATRACE_NAME("BlurFilter::prepare");

    glActiveTexture(GL_TEXTURE0);

    // Convert from sRGB to linear color space
    GLFramebuffer* firstBuf = mPassFbos[0];
    mColorSpaceProgram.useProgram();
    glViewport(0, 0, firstBuf->getBufferWidth(), firstBuf->getBufferHeight());
    glBindTexture(GL_TEXTURE_2D, mCompositionFbo.getTextureName());
    firstBuf->bind();
    glUniform1i(mCTextureLoc, 0);
    drawMesh(mCVertexArray);

    ALOGI("SARU: prepare - initial dims %dx%d", mPassFbos[0]->getBufferWidth(), mPassFbos[0]->getBufferHeight());

    // Set up downsampling shader
    mDownsampleProgram.useProgram();
    glUniform1i(mDTextureLoc, 0);
    glUniform1f(mDOffsetLoc, mOffset);

    GLFramebuffer* read;
    GLFramebuffer* draw;

    // Downsample
    for (auto i = 0; i < mPasses; i++) {
        ATRACE_NAME("BlurFilter::renderDownsamplePass");

        // Skip FBO 0 to avoid unnecessary blit
        read = mPassFbos[i];
        draw = mPassFbos[i + 1];

        renderPass(read, draw, mDHalfPixelLoc, mDVertexArray);
    }

    // Set up upsampling shader
    mUpsampleProgram.useProgram();
    glUniform1i(mUTextureLoc, 0);
    glUniform1f(mUOffsetLoc, mOffset);

    // Upsample
    for (auto i = 0; i < mPasses; i++) {
        ATRACE_NAME("BlurFilter::renderUpsamplePass");

        // Upsampling uses buffers in the reverse direction
        read = mPassFbos[mPasses - i];
        draw = mPassFbos[mPasses - i - 1];

        renderPass(read, draw, mUHalfPixelLoc, mUVertexArray);
    }

    mLastDrawTarget = draw;
    return NO_ERROR;
}

status_t BlurFilter::render(bool /*multiPass*/) {
    ATRACE_NAME("BlurFilter::render");

    // Now let's scale our blur up. It will be interpolated with the larger composited
    // texture for the first frames, to hide downscaling artifacts.
    GLfloat opacity = fmin(1.0, mRadius / kMaxCrossFadeRadius);

    // When doing multiple passes, we cannot try to read mCompositionFbo, given that we'll
    // be writing onto it. Let's disable the crossfade, otherwise we'd need 1 extra frame buffer,
    // as large as the screen size.
    //if (opacity >= 1 || multiPass) {
    //    mLastDrawTarget->bindAsReadBuffer();
    //    glBlitFramebuffer(0, 0, mLastDrawTarget->getBufferWidth(),
    //                      mLastDrawTarget->getBufferHeight(), mDisplayX, mDisplayY, mDisplayWidth,
    //                      mDisplayHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    //    return NO_ERROR;
    //}

    // Crossfade using mix shader
    mMixProgram.useProgram();
    glUniform1f(mMBlurOpacityLoc, opacity);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mCompositionFbo.getTextureName());
    glUniform1i(mMCompositionTextureLoc, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mLastDrawTarget->getTextureName());
    glUniform1i(mMBlurredTextureLoc, 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, mDitherFbo.getTextureName());
    glUniform1i(mMDitherTextureLoc, 2);

    drawMesh(mMVertexArray);

    // Clean up to avoid breaking further composition
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    mEngine.checkErrors("Mixing blur");
    return NO_ERROR;
}

string BlurFilter::getVertexShader() const {
    return R"SHADER(
        #version 310 es
        precision mediump float;

        in vec2 aPosition;
        in highp vec2 aUV;
        out highp vec2 vUV;

        void main() {
            vUV = aUV;
            gl_Position = vec4(aPosition, 0.0, 1.0);
        }
    )SHADER";
}

string BlurFilter::getColorSpaceFragShader() const {
    return R"SHADER(
        #version 310 es
        precision mediump float;

        uniform sampler2D uTexture;

        in highp vec2 vUV;
        out vec4 fragColor;

        vec3 srgbToLinearRgb(vec3 srgb) {
            vec3 linearRgb = srgb * 12.92;
            vec3 gammaRgb = (pow(srgb, vec3(1.0 / 2.4)) * 1.055) - 0.055;
            bvec3 selectParts = lessThan(srgb, vec3(0.0031308));
            return mix(gammaRgb, linearRgb, selectParts);
        }

        vec3 linearRgbToSrgb(vec3 linear) {
            vec3 srgbLinear = linear / 12.92;
            vec3 srgbGamma = pow((linear + 0.055) / 1.055, vec3(2.4));
            bvec3 selectParts = lessThan(linear, vec3(0.04045));
            return mix(srgbGamma, srgbLinear, selectParts);
        }

        vec3 linearRgbToOklab(vec3 linear)
        {
            vec3 lms = vec3(
                dot(vec3(0.4121656120, 0.5362752080, 0.0514575653), linear),
                dot(vec3(0.2118591070, 0.6807189584, 0.1074065790), linear),
                dot(vec3(0.0883097947, 0.2818474174, 0.6302613616), linear)
            );
            vec3 lms_ = pow(lms, vec3(1.0 / 3.0));

            return vec3(
                dot(vec3(+0.2104542553, +0.7936177850, -0.0040720468), lms_),
                dot(vec3(+1.9779984951, -2.4285922050, +0.4505937099), lms_),
                dot(vec3(+0.0259040371, +0.7827717662, -0.8086757660), lms_)
            );
        }

        vec3 oklabToLinearRgb(vec3 oklab)
        {
            // rgb = Lab
            vec3 lms_ = vec3(
                dot(vec3(+1.0, +0.3963377774, +0.2158037573), oklab),
                dot(vec3(+1.0, -0.1055613458, -0.0638541728), oklab),
                dot(vec3(+1.0, -0.0894841775, -1.2914855480), oklab)
            );
            vec3 lms = lms_ * lms_ * lms_;

            return vec3(
                dot(vec3(+4.0767245293, -3.3072168827, +0.2307590544), lms),
                dot(vec3(-1.2681437731, +2.6093323231, -0.3411344290), lms),
                dot(vec3(-0.0041119885, -0.7034763098, +1.7068625689), lms)
            );
        }

        void main() {
            vec3 linear = srgbToLinearRgb(texture(uTexture, vUV).rgb);
            vec3 oklab = linearRgbToOklab(linear);
            fragColor = vec4(oklab, 1.0);
        }
    )SHADER";
}

string BlurFilter::getDownsampleFragShader() const {
    return R"SHADER(
        #version 310 es
        precision mediump float;

        uniform sampler2D uTexture;
        uniform float uOffset;
        uniform vec2 uHalfPixel;

        in highp vec2 vUV;
        out vec4 fragColor;

        void main() {
            vec4 sum = texture(uTexture, vUV) * 4.0;
            sum += texture(uTexture, vUV - uHalfPixel.xy * uOffset);
            sum += texture(uTexture, vUV + uHalfPixel.xy * uOffset);
            sum += texture(uTexture, vUV + vec2(uHalfPixel.x, -uHalfPixel.y) * uOffset);
            sum += texture(uTexture, vUV - vec2(uHalfPixel.x, -uHalfPixel.y) * uOffset);
            fragColor = sum / 8.0;
        }
    )SHADER";
}

string BlurFilter::getUpsampleFragShader() const {
    return R"SHADER(
        #version 310 es
        precision mediump float;

        uniform sampler2D uTexture;
        uniform float uOffset;
        uniform vec2 uHalfPixel;

        in highp vec2 vUV;
        out vec4 fragColor;

        void main() {
            vec4 sum = texture(uTexture, vUV + vec2(-uHalfPixel.x * 2.0, 0.0) * uOffset);
            sum += texture(uTexture, vUV + vec2(-uHalfPixel.x, uHalfPixel.y) * uOffset) * 2.0;
            sum += texture(uTexture, vUV + vec2(0.0, uHalfPixel.y * 2.0) * uOffset);
            sum += texture(uTexture, vUV + vec2(uHalfPixel.x, uHalfPixel.y) * uOffset) * 2.0;
            sum += texture(uTexture, vUV + vec2(uHalfPixel.x * 2.0, 0.0) * uOffset);
            sum += texture(uTexture, vUV + vec2(uHalfPixel.x, -uHalfPixel.y) * uOffset) * 2.0;
            sum += texture(uTexture, vUV + vec2(0.0, -uHalfPixel.y * 2.0) * uOffset);
            sum += texture(uTexture, vUV + vec2(-uHalfPixel.x, -uHalfPixel.y) * uOffset) * 2.0;
            fragColor = sum / 12.0;
        }
    )SHADER";
}

string BlurFilter::getMixFragShader() const {
    return R"SHADER(
        #version 310 es
        precision mediump float;

        uniform sampler2D uCompositionTexture;
        uniform sampler2D uBlurredTexture;
        uniform sampler2D uDitherTexture;
        uniform float uBlurOpacity;

        in highp vec2 vUV;
        out vec4 fragColor;

        vec3 srgbToLinearRgb(vec3 srgb) {
            vec3 linearRgb = srgb * 12.92;
            vec3 gammaRgb = (pow(srgb, vec3(1.0 / 2.4)) * 1.055) - 0.055;
            bvec3 selectParts = lessThan(srgb, vec3(0.0031308));
            return mix(gammaRgb, linearRgb, selectParts);
        }

        vec3 linearRgbToSrgb(vec3 linear) {
            vec3 srgbLinear = linear / 12.92;
            vec3 srgbGamma = pow((linear + 0.055) / 1.055, vec3(2.4));
            bvec3 selectParts = lessThan(linear, vec3(0.04045));
            return mix(srgbGamma, srgbLinear, selectParts);
        }

        vec3 linearRgbToOklab(vec3 linear)
        {
            vec3 lms = vec3(
                dot(vec3(0.4121656120, 0.5362752080, 0.0514575653), linear),
                dot(vec3(0.2118591070, 0.6807189584, 0.1074065790), linear),
                dot(vec3(0.0883097947, 0.2818474174, 0.6302613616), linear)
            );
            vec3 lms_ = pow(lms, vec3(1.0 / 3.0));

            return vec3(
                dot(vec3(+0.2104542553, +0.7936177850, -0.0040720468), lms_),
                dot(vec3(+1.9779984951, -2.4285922050, +0.4505937099), lms_),
                dot(vec3(+0.0259040371, +0.7827717662, -0.8086757660), lms_)
            );
        }

        vec3 oklabToLinearRgb(vec3 oklab)
        {
            // rgb = Lab
            vec3 lms_ = vec3(
                dot(vec3(+1.0, +0.3963377774, +0.2158037573), oklab),
                dot(vec3(+1.0, -0.1055613458, -0.0638541728), oklab),
                dot(vec3(+1.0, -0.0894841775, -1.2914855480), oklab)
            );
            vec3 lms = lms_ * lms_ * lms_;

            return vec3(
                dot(vec3(+4.0767245293, -3.3072168827, +0.2307590544), lms),
                dot(vec3(-1.2681437731, +2.6093323231, -0.3411344290), lms),
                dot(vec3(-0.0041119885, -0.7034763098, +1.7068625689), lms)
            );
        }

        void main() {
            vec4 blurred = texture(uBlurredTexture, vUV);
            blurred = vec4(linearRgbToSrgb(oklabToLinearRgb(blurred.rgb)), 1.0);
            vec4 composition = texture(uCompositionTexture, vUV);

            // First /64: screen coordinates -> texture coordinates (UV)
            // Second /64: reduce magnitude to make it a dither instead of an overlay (from Bayer 8x8)
            vec3 dither = texture(uDitherTexture, gl_FragCoord.xy / 64.0).rgb / 64.0;
            blurred = vec4(blurred.rgb + dither, 1.0);

            fragColor = mix(composition, blurred, 1.0);
        }
    )SHADER";
}

} // namespace gl
} // namespace renderengine
} // namespace android

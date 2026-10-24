#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "../games/acnh/runtime/acnh_msl_normalize.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z, w; } rgba_t;

static id<MTLComputePipelineState> pipeline(id<MTLDevice> device, NSString *source) {
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    if (@available(macOS 15.0, *)) options.mathMode = MTLMathModeSafe;
    else options.fastMathEnabled = NO;
    options.preserveInvariance = YES;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    if (!library) { fprintf(stderr, "Metal compile failed: %s\n", error.localizedDescription.UTF8String); return nil; }
    id<MTLFunction> function = [library newFunctionWithName:@"probe"];
    id<MTLComputePipelineState> result = [device newComputePipelineStateWithFunction:function error:&error];
    if (!result) fprintf(stderr, "Metal pipeline failed: %s\n", error.localizedDescription.UTF8String);
    return result;
}

static int sample(id<MTLDevice> device, id<MTLTexture> texture, NSString *source, rgba_t result[2]) {
    id<MTLComputePipelineState> state = pipeline(device, source);
    if (!state) return 0;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> buffer = [device newBufferWithLength:sizeof(rgba_t) * 2 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:state]; [encoder setTexture:texture atIndex:0]; [encoder setBuffer:buffer offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "Metal dispatch failed: %s\n", command.error.localizedDescription.UTF8String); return 0; }
    memcpy(result, buffer.contents, sizeof(rgba_t) * 2);
    return 1;
}

static NSString *source_text(void) {
    return @"using namespace metal;\n"
             "float4 sample_at(texture2d_array<float> tex, sampler s, float layer) { return tex.sample(s, float2(0.5), uint(rint(layer))); }\n"
             "kernel void probe(texture2d_array<float> tex [[texture(0)]], device float4 *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
             "sampler s(coord::normalized, address::clamp_to_edge, filter::nearest); float layer = i == 0 ? 1.0 : 7.0; out[i] = sample_at(tex, s, layer); }\n";
}

static int color(rgba_t v, float r, float g, float b) {
    return fabsf(v.x-r) < 0.02f && fabsf(v.y-g) < 0.02f && fabsf(v.z-b) < 0.02f;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "FAIL: no Metal device\n"); return 1; }
        MTLTextureDescriptor *baseDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        baseDesc.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> base = [device newTextureWithDescriptor:baseDesc];
        const uint8_t red[4] = {255, 0, 0, 255};
        [base replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:red bytesPerRow:4];
        id<MTLTexture> single = [base newTextureViewWithPixelFormat:base.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
        if (!single) { fprintf(stderr, "FAIL: cannot create production-shaped single-layer array view\n"); return 1; }

        rgba_t result[2]; NSString *raw = source_text();
        if (!sample(device, single, raw, result)) return 1;
        if (!color(result[0], 0, 0, 0)) { fprintf(stderr, "FAIL: required negative control did not return zero (%.3f, %.3f, %.3f, %.3f)\n", result[0].x, result[0].y, result[0].z, result[0].w); return 1; }
        char *normalized = NULL, message[192] = {0}; size_t normalized_length = 0;
        if (acnh_msl_normalize(raw.UTF8String, strlen(raw.UTF8String), &normalized, &normalized_length, message, sizeof message) != ACNH_MSL_CHANGED) {
            fprintf(stderr, "FAIL: normalizer: %s\n", message); return 1;
        }
        NSString *fixed = [[NSString alloc] initWithBytes:normalized length:normalized_length encoding:NSUTF8StringEncoding]; free(normalized);
        if (!sample(device, single, fixed, result)) return 1;
        if (!color(result[0], 1, 0, 0) || !color(result[1], 1, 0, 0)) { fprintf(stderr, "FAIL: normalized single-layer view did not sample red\n"); return 1; }

        MTLTextureDescriptor *arrayDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        arrayDesc.textureType = MTLTextureType2DArray; arrayDesc.arrayLength = 2; arrayDesc.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> array = [device newTextureWithDescriptor:arrayDesc];
        const uint8_t green[4] = {0, 255, 0, 255};
        [array replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:0 withBytes:red bytesPerRow:4 bytesPerImage:4];
        [array replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:1 withBytes:green bytesPerRow:4 bytesPerImage:4];
        if (!sample(device, array, fixed, result)) return 1;
        if (!color(result[0], 0, 1, 0) || !color(result[1], 0, 1, 0)) { fprintf(stderr, "FAIL: two-layer array did not preserve/clamp to green layer 1\n"); return 1; }
        puts("ACNH MSL Metal probe passed (negative control, one-layer clamp, two-layer preservation and clamp)");
    }
    return 0;
}

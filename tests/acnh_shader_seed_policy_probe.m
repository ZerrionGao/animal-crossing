#import <Foundation/Foundation.h>

#include "../engine/graphics/nvn/totk_shader_seed.h"

static void require(BOOL condition, NSString *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message.UTF8String); exit(1); }
}

static NSString *write_seed(NSString *root, NSString *policy) {
    NSFileManager *fm = NSFileManager.defaultManager;
    [fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *variant = [@"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" stringByAppendingString:@"-bbbbbbbbbbbb_b"];
    NSDictionary *contents = @{
        [variant stringByAppendingString:@".metal"]: @"shader source",
        [variant stringByAppendingString:@".json"]: @"{}",
        @"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.variants": @"[]",
        @"pso_manifest.tsv": @"# probe\n",
        @"shader-keys.json": @"[]",
        [variant stringByAppendingString:@".metallib"]: @"seed metallib"
    };
    NSMutableDictionary *hashes = [NSMutableDictionary dictionary];
    [contents enumerateKeysAndObjectsUsingBlock:^(NSString *name, NSString *text, BOOL *stop) {
        NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
        [data writeToFile:[root stringByAppendingPathComponent:name] atomically:YES];
        hashes[name] = totk_seed_hash(data);
    }];
    NSMutableDictionary *manifest = [@{@"schema": @1, @"chain": @DMSH_CHAIN, @"shaders": @1, @"files": hashes} mutableCopy];
    if (policy) manifest[@"shader_policy"] = policy;
    NSData *json = [NSJSONSerialization dataWithJSONObject:manifest options:NSJSONWritingPrettyPrinted error:nil];
    [json writeToFile:[root stringByAppendingPathComponent:@"seed-manifest.json"] atomically:YES];
    return variant;
}

int main(void) {
    @autoreleasepool {
        NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSUUID.UUID.UUIDString copy]];
        NSString *source = [root stringByAppendingPathComponent:@"source"], *cache = [root stringByAppendingPathComponent:@"cache"];
        NSString *marker = [cache stringByAppendingPathComponent:@"seed-installed.json"];
        NSString *variant = write_seed(source, nil); NSError *error = nil;
        require(totk_seed_import_into(source, cache, marker, &error) == nil, @"missing policy is rejected");
        write_seed(source, @"old-policy"); error = nil;
        require(totk_seed_import_into(source, cache, marker, &error) == nil, @"unsupported policy is rejected");

        variant = write_seed(source, @"acnh-metal-v1"); error = nil;
        NSDictionary *import = totk_seed_import_into(source, cache, marker, &error);
        require(import && [import[@"available"] boolValue], error.localizedDescription ?: @"valid policy imports");
        NSDictionary *receipt = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:marker] options:0 error:nil];
        require([receipt[@"shader_policy"] isEqual:@"acnh-metal-v1"], @"receipt records policy");

        NSString *cacheMetal = [cache stringByAppendingPathComponent:[variant stringByAppendingString:@".metal"]];
        NSString *cacheJson = [cache stringByAppendingPathComponent:[variant stringByAppendingString:@".json"]];
        NSString *cacheIndex = [cache stringByAppendingPathComponent:@"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.variants"];
        NSString *cacheLib = [cache stringByAppendingPathComponent:[variant stringByAppendingString:@".metallib"]];
        [@"user raw source" writeToFile:cacheMetal atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [@"user json" writeToFile:cacheJson atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [@"[{\"variant\":\"other\",\"deps\":{},\"note\":\"user index\"}]" writeToFile:cacheIndex atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [@"old policy library" writeToFile:cacheLib atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [@{@"files_sha256": @"old-fingerprint", @"shader_policy": @"old-policy"} writeToFile:marker atomically:YES];
        error = nil; import = totk_seed_import_into(source, cache, marker, &error);
        require(import && [import[@"available"] boolValue], error.localizedDescription ?: @"policy mismatch reimports");
        require([[NSString stringWithContentsOfFile:cacheMetal encoding:NSUTF8StringEncoding error:nil] isEqual:@"user raw source"], @"raw MSL is retained");
        require([[NSString stringWithContentsOfFile:cacheJson encoding:NSUTF8StringEncoding error:nil] isEqual:@"user json"], @"raw reflection JSON is retained");
        require([[NSString stringWithContentsOfFile:cacheIndex encoding:NSUTF8StringEncoding error:nil] containsString:@"user index"], @"variant index is retained");
        require([[NSString stringWithContentsOfFile:cacheLib encoding:NSUTF8StringEncoding error:nil] isEqual:@"seed metallib"], @"stale metallib is replaced");
        receipt = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:marker] options:0 error:nil];
        require([receipt[@"shader_policy"] isEqual:@"acnh-metal-v1"], @"new receipt records policy");
        [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    }
    puts("ACNH shader seed policy probe passed");
    return 0;
}

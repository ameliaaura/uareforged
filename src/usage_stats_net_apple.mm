// Anonymous usage stats: the macOS sender (NSURLSession, Foundation; already loaded by SDL).
// Ephemeral session: no cookies, no cache, no stored credentials; fixed User-Agent and
// Accept-Language, so the request says nothing about the Mac or its language.
#import <Foundation/Foundation.h>

#include "usage_stats.h"

namespace UsageStats
{

int DefaultSend(const std::string &url, const std::string &body, int timeoutMs)
{
    __block int status = -1;
    @autoreleasepool
    {
        NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if ( !u )
            return -1;
        NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest = timeoutMs / 1000.0;
        cfg.timeoutIntervalForResource = timeoutMs / 1000.0;
        cfg.HTTPCookieStorage = nil;
        cfg.HTTPShouldSetCookies = NO;
        cfg.URLCache = nil;
        cfg.URLCredentialStorage = nil;
        cfg.HTTPAdditionalHeaders = @{@"User-Agent" : @"UA-Reforged", @"Accept-Language" : @"en"};
        NSURLSession *session = [NSURLSession sessionWithConfiguration:cfg];
        NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:u];
        req.HTTPMethod = @"POST";
        [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        [req setValue:@"UA-Reforged" forHTTPHeaderField:@"User-Agent"];
        req.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        NSURLSessionDataTask *task = [session dataTaskWithRequest:req
                                                completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
                                                    (void)data;
                                                    if ( !err && [resp isKindOfClass:[NSHTTPURLResponse class]] )
                                                        status = (int)((NSHTTPURLResponse *)resp).statusCode;
                                                    dispatch_semaphore_signal(done);
                                                }];
        [task resume];
        if ( dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeoutMs + 500) * NSEC_PER_MSEC)) != 0 )
        {
            [task cancel];
            status = -2;
            [session finishTasksAndInvalidate];
            return -2; // the handler may still run later: `done` is left to it (a few bytes)
        }
        [session finishTasksAndInvalidate];
#if !__has_feature(objc_arc)
        dispatch_release(done);
#endif
    }
    return status;
}

} // namespace UsageStats

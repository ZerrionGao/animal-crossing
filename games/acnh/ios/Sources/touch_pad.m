#import "touch_pad.h"
#import <GameController/GameController.h>
#include <math.h>
#include "runtime.h"

/* 按键与摇杆布局限制在游戏画面和安全区域的交集内。空白处触摸仍可操作对应侧的摇杆；
   连接手柄时，最后一次触摸结束后自动隐藏覆盖层。 */
typedef struct { CGPoint at; uint64_t bit; NSString *label; CGFloat size; } TouchButton;
enum { kButtonCount = 14 };
static const NSTimeInterval kHideAfter = 3;   // seconds after the last finger lifts, with a controller connected

@implementation TOTKTouchPad {
    NSMapTable<UITouch *, NSValue *> *points;
    NSMapTable<UITouch *, NSValue *> *leftOrigins;    // where each left-stick touch went down
    NSMapTable<UITouch *, NSValue *> *rightOrigins;   // the same for the camera stick
    BOOL gamepad, concealed;                          // a controller is connected; the layer is hidden for it
    NSTimer *hideTimer;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.multipleTouchEnabled = YES; self.backgroundColor = UIColor.clearColor; self.contentMode = UIViewContentModeRedraw;
        points = [NSMapTable strongToStrongObjectsMapTable];
        leftOrigins = [NSMapTable strongToStrongObjectsMapTable];
        rightOrigins = [NSMapTable strongToStrongObjectsMapTable];
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        [center addObserver:self selector:@selector(controllersChanged:) name:GCControllerDidConnectNotification object:nil];
        [center addObserver:self selector:@selector(controllersChanged:) name:GCControllerDidDisconnectNotification object:nil];
        [center addObserver:self selector:@selector(clearTouches:) name:UIApplicationWillResignActiveNotification object:nil];
        [self controllersChanged:nil];
    }
    return self;
}

- (void)controllersChanged:(NSNotification *)note {
    BOOL connected = NO;
    for (GCController *controller in GCController.controllers)
        if (controller.extendedGamepad && !([note.name isEqualToString:GCControllerDidDisconnectNotification] && controller == note.object)) connected = YES;
    if (connected == gamepad) return;
    gamepad = connected; [hideTimer invalidate]; hideTimer = nil;
    if (gamepad) [self conceal]; else { concealed = NO; [self setNeedsDisplay]; }
}

- (void)conceal {
    concealed = YES;
    // Fingers on the screen when it hides would otherwise hold their buttons and sticks.
    [points removeAllObjects]; [leftOrigins removeAllObjects]; [rightOrigins removeAllObjects]; [self updateState];
}

- (void)hideLater {
    [hideTimer invalidate]; hideTimer = nil;
    if (!gamepad || points.count) return;
    __weak TOTKTouchPad *weakSelf = self;
    hideTimer = [NSTimer scheduledTimerWithTimeInterval:kHideAfter repeats:NO block:^(NSTimer *timer) {
        (void)timer; TOTKTouchPad *pad = weakSelf; if (pad) [pad concealIfIdle];
    }];
}
- (void)concealIfIdle { if (gamepad && !points.count) [self conceal]; }

- (CGRect)area {
    CGRect safe = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
    CGFloat width = MIN(self.bounds.size.width, self.bounds.size.height * 16.0 / 9.0);
    CGRect game = CGRectMake(CGRectGetMidX(self.bounds) - width / 2, CGRectGetMinY(self.bounds), width, self.bounds.size.height);
    return CGRectIntersection(safe, game);
}
- (CGFloat)stickRadius { return MIN(62, self.area.size.height * .15); }
- (CGFloat)diamond { return MIN(52, self.area.size.height * .14); }
- (CGFloat)hitRadius { return MIN(36, self.diamond * .68); }
- (CGFloat)columnInset { return self.diamond + 33; }
- (CGPoint)restingStick:(BOOL)right {
    CGRect a = self.area; CGFloat inset = self.columnInset + self.stickRadius * 1.45;
    return CGPointMake(right ? CGRectGetMaxX(a) - inset : CGRectGetMinX(a) + inset, CGRectGetMinY(a) + a.size.height * .80);
}

- (void)buttons:(TouchButton *)b {
    CGRect a = self.area; CGFloat x = CGRectGetMinX(a), y = CGRectGetMinY(a), w = a.size.width, h = a.size.height, d = self.diamond;
    CGFloat top = y + h * .12, bottom = y + h * .88;
    CGFloat inset = self.columnInset;
    CGPoint face = CGPointMake(x + w - inset, y + h * .42), pad = CGPointMake(x + inset, y + h * .42);
    TouchButton all[kButtonCount] = {
        {{face.x + d, face.y}, 1, @"A", 54}, {{face.x, face.y + d}, 2, @"B", 54}, {{face.x, face.y - d}, 4, @"X", 54}, {{face.x - d, face.y}, 8, @"Y", 54},
        {{x + 32, top}, 256, @"ZL", 52}, {{x + 96, top}, 64, @"L", 52}, {{x + w - 96, top}, 128, @"R", 52}, {{x + w - 32, top}, 512, @"ZR", 52},
        {{x + w * .39, bottom}, 2048, @"−", 40}, {{x + w * .61, bottom}, 1024, @"+", 40},
        {{pad.x - d, pad.y}, 4096, @"◀", 40}, {{pad.x, pad.y - d}, 8192, @"▲", 40}, {{pad.x + d, pad.y}, 16384, @"▶", 40}, {{pad.x, pad.y + d}, 32768, @"▼", 40}};
    for (int i = 0; i < kButtonCount; i++) b[i] = all[i];
}

- (uint64_t)buttonAt:(CGPoint)p {
    TouchButton b[kButtonCount]; [self buttons:b]; CGFloat hit = self.hitRadius; uint64_t bits = 0;
    for (int i = 0; i < kButtonCount; i++) {
        CGFloat radius = MAX(22, MIN(hit, b[i].size * .65));
        if (i >= 4 && i <= 7) radius = MIN(radius, 30);
        if (hypot(p.x - b[i].at.x, p.y - b[i].at.y) < radius) bits |= b[i].bit;
    }
    return bits;
}

static void stick_axes(CGPoint from, CGPoint to, CGFloat radius, int32_t *x, int32_t *y) {
    CGFloat dx = to.x - from.x, dy = from.y - to.y, dist = hypot(dx, dy);
    if (dist > radius) { dx *= radius / dist; dy *= radius / dist; }
    *x = (int32_t)(dx / radius * 32767); *y = (int32_t)(dy / radius * 32767);
}

- (void)updateState {
    RuntimeGamepadState s = {.connected = 1}; CGFloat radius = self.stickRadius;
    for (UITouch *t in points.keyEnumerator) {
        CGPoint p = [points objectForKey:t].CGPointValue; NSValue *left = [leftOrigins objectForKey:t], *right = [rightOrigins objectForKey:t];
        if (left) stick_axes(left.CGPointValue, p, radius, &s.lx, &s.ly);
        else if (right) stick_axes(right.CGPointValue, p, radius, &s.rx, &s.ry);
        else s.buttons |= [self buttonAt:p];
    }
    runtime_touch_pad_set(&s); [self setNeedsDisplay];
}

- (void)clearTouches:(NSNotification *)note {
    (void)note;
    [hideTimer invalidate]; hideTimer = nil;
    [points removeAllObjects]; [leftOrigins removeAllObjects]; [rightOrigins removeAllObjects];
    [self updateState];
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event; concealed = NO; [hideTimer invalidate]; hideTimer = nil;
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self]; NSValue *v = [NSValue valueWithCGPoint:p]; [points setObject:v forKey:t];
        if ([self buttonAt:p]) continue;
        [(p.x < CGRectGetMidX(self.bounds) ? leftOrigins : rightOrigins) setObject:v forKey:t];
    }
    [self updateState];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *t in touches) [points setObject:[NSValue valueWithCGPoint:[t locationInView:self]] forKey:t];
    [self updateState];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *t in touches) { [points removeObjectForKey:t]; [leftOrigins removeObjectForKey:t]; [rightOrigins removeObjectForKey:t]; }
    [self updateState]; [self hideLater];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self touchesEnded:touches withEvent:event]; }
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsDisplay]; }

/* 细双环和四枚叶片刻纹为摇杆提供装饰，不改变摇杆输入范围。 */
static void draw_stick(CGContextRef c, CGPoint o, CGPoint p, CGFloat r) {
    int32_t x, y; stick_axes(o, p, r, &x, &y);
    CGContextSaveGState(c);
    CGContextSetStrokeColorWithColor(c, [UIColor colorWithRed:.88 green:.87 blue:.64 alpha:.55].CGColor);
    CGContextSetLineWidth(c, 1.2);
    CGContextStrokeEllipseInRect(c, CGRectMake(o.x-r, o.y-r, 2*r, 2*r));
    CGContextStrokeEllipseInRect(c, CGRectMake(o.x-r+4, o.y-r+4, 2*r-8, 2*r-8));
    for (int i = 0; i < 4; i++) {
        CGContextSaveGState(c); CGContextTranslateCTM(c, o.x, o.y); CGContextRotateCTM(c, i * M_PI_2);
        CGContextMoveToPoint(c, 0, -r-4); CGContextAddQuadCurveToPoint(c, 7, -r, 0, -r+4);
        CGContextAddQuadCurveToPoint(c, -7, -r, 0, -r-4); CGContextStrokePath(c); CGContextRestoreGState(c);
    }
    CGPoint k = CGPointMake(o.x + x / 32767.0 * r, o.y - y / 32767.0 * r);
    CGRect knob = CGRectMake(k.x - 22, k.y - 22, 44, 44);
    CGContextSetFillColorWithColor(c, [UIColor colorWithRed:.12 green:.19 blue:.14 alpha:.62].CGColor);
    CGContextFillEllipseInRect(c, knob); CGContextStrokeEllipseInRect(c, knob);
    CGContextStrokeEllipseInRect(c, CGRectInset(knob, 4, 4)); CGContextRestoreGState(c);
}

- (void)drawRect:(CGRect)rect {
    if (concealed) return;
    (void)rect; CGContextRef c = UIGraphicsGetCurrentContext();
    CGFloat r = self.stickRadius;
    if (!leftOrigins.count) { CGPoint rest = [self restingStick:NO]; draw_stick(c, rest, rest, r); }
    if (!rightOrigins.count) { CGPoint rest = [self restingStick:YES]; draw_stick(c, rest, rest, r); }
    for (UITouch *t in leftOrigins.keyEnumerator) draw_stick(c, [leftOrigins objectForKey:t].CGPointValue, [points objectForKey:t].CGPointValue, r);
    for (UITouch *t in rightOrigins.keyEnumerator) draw_stick(c, [rightOrigins objectForKey:t].CGPointValue, [points objectForKey:t].CGPointValue, r);
    uint64_t pressed = 0;
    for (UITouch *t in points.keyEnumerator)
        if (![leftOrigins objectForKey:t] && ![rightOrigins objectForKey:t]) pressed |= [self buttonAt:[points objectForKey:t].CGPointValue];
    TouchButton b[kButtonCount]; [self buttons:b];
    for (int i = 0; i < kButtonCount; i++) {
        CGFloat size = b[i].size; CGRect box = CGRectMake(b[i].at.x - size / 2, b[i].at.y - size / 2, size, size);
        BOOL active = (pressed & b[i].bit) != 0;
        UIColor *fill = active ? [UIColor colorWithRed:.35 green:.50 blue:.30 alpha:.86] : [UIColor colorWithRed:.10 green:.17 blue:.13 alpha:.72];
        UIColor *edge = [UIColor colorWithRed:.91 green:.87 blue:.65 alpha:active ? .95 : .65];
        CGContextSetFillColorWithColor(c, fill.CGColor); CGContextFillEllipseInRect(c, box);
        CGContextSetStrokeColorWithColor(c, edge.CGColor); CGContextSetLineWidth(c, 1.2); CGContextStrokeEllipseInRect(c, box);
        CGContextSetLineWidth(c, .6); CGContextStrokeEllipseInRect(c, CGRectInset(box, 3, 3));
        CGFloat fontSize = size >= 50 ? 20 : (size < 40 ? 13 : 15);
        NSDictionary *attrs = @{NSFontAttributeName: [UIFont systemFontOfSize:fontSize weight:UIFontWeightSemibold], NSForegroundColorAttributeName: [UIColor colorWithRed:1 green:.98 blue:.88 alpha:.95]};
        CGSize z = [b[i].label sizeWithAttributes:attrs]; [b[i].label drawAtPoint:CGPointMake(b[i].at.x - z.width / 2, b[i].at.y - z.height / 2) withAttributes:attrs];
    }
}

@end

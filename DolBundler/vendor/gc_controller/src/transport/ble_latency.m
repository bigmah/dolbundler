//  Asks macOS for a faster BLE connection interval.
//
//  The connection interval is what bounds wireless input latency: a report can
//  only reach the host on a connection event, so the interval *is* the worst
//  case. macOS hands this controller 15 ms, which is Apple's default for a
//  generic GATT accessory — the controller does not expose HID over GATT, so it
//  never qualifies for bluetoothd's LE-HID fast path.
//
//  There is exactly one lever reachable from an unentitled process.
//  CoreBluetooth ships `-[CBCentralManager setDesiredConnectionLatency:
//  forPeripheral:]`, the central-role counterpart to the public
//  `-[CBPeripheralManager setDesiredConnectionLatency:forCentral:]`. It is SPI,
//  so we reach it by selector rather than by header.
//
//  Raw HCI is *not* an option and this file deliberately does not attempt it:
//  `IOServiceOpen` on IOBluetoothHCIController returns kIOReturnUnsupported
//  (0xe00002c7) without `com.apple.bluetooth.iokit-user-access`, which only
//  Apple-signed binaries carry. IOBluetoothHostController's HCI methods are
//  still callable, but they silently return 0 and fill nothing.
//
//  btleplug owns the CBCentralManager, so we swizzle rather than fork it. The
//  request is made twice on purpose:
//
//    - in `connectPeripheral:options:`, so the preference is on record before
//      the link parameters are first chosen;
//    - in the delegate's `centralManager:didConnectPeripheral:`, which is the
//      call that actually takes, and which runs on CoreBluetooth's own queue.
//
//  That second point is why this is a delegate swizzle and not a timer: calling
//  into CoreBluetooth from an unrelated thread is not safe, and the delegate
//  callback puts us on the right queue for free.

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <os/lock.h>
#import <stdatomic.h>
#import <string.h>

/// Desired latency class, or -1 to leave macOS's choice alone.
static int g_level = -1;

static atomic_int g_applied;
static atomic_int g_failed;

/// Last payload from `handleConnectionParametersUpdated:`, for diagnostics.
/// Deliberately surfaced as text: the units CoreBluetooth uses here are not
/// documented, and guessing at them would be worse than showing the raw value.
/// The inter-arrival histogram is the authoritative measurement.
static char g_params[512];
static os_unfair_lock g_params_lock = OS_UNFAIR_LOCK_INIT;

static IMP g_orig_connect;
static IMP g_orig_did_connect;
static IMP g_orig_params;

static SEL set_latency_sel(void) {
    return sel_registerName("setDesiredConnectionLatency:forPeripheral:");
}

/// Replaces `sel`'s implementation on `cls`, returning the previous one.
static IMP swizzle(Class cls, SEL sel, IMP replacement) {
    if (!cls) {
        return NULL;
    }
    Method m = class_getInstanceMethod(cls, sel);
    return m ? method_setImplementation(m, replacement) : NULL;
}

static void apply_latency(id central, id peripheral) {
    if (g_level < 0 || !central || !peripheral) {
        return;
    }
    SEL sel = set_latency_sel();
    if (![central respondsToSelector:sel]) {
        atomic_fetch_add(&g_failed, 1);
        return;
    }
    @try {
        ((void (*)(id, SEL, NSInteger, id))objc_msgSend)(
            central, sel, (NSInteger)g_level, peripheral);
        atomic_fetch_add(&g_applied, 1);
    } @catch (NSException *e) {
        atomic_fetch_add(&g_failed, 1);
    }
}

static void record_params(id info) {
    if (!info) {
        return;
    }
    @try {
        NSString *text = [info description];
        if (!text) {
            return;
        }
        const char *utf8 = [text UTF8String];
        if (!utf8) {
            return;
        }
        os_unfair_lock_lock(&g_params_lock);
        strncpy(g_params, utf8, sizeof(g_params) - 1);
        g_params[sizeof(g_params) - 1] = '\0';
        os_unfair_lock_unlock(&g_params_lock);
    } @catch (NSException *e) {
        // Diagnostics only — never let this disturb the link.
    }
}

/// btleplug declares its delegate lazily, so the class does not exist until it
/// builds a manager. Installing this on the first connect is early enough: the
/// callback we want happens strictly later.
static void gc_ble_swizzled_did_connect(id self, SEL _cmd, id central, id peripheral);

static void ensure_delegate_swizzled(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Class delegate = objc_getClass("BtlePlugCentralManagerDelegate");
        g_orig_did_connect = swizzle(delegate,
                                     sel_registerName("centralManager:didConnectPeripheral:"),
                                     (IMP)gc_ble_swizzled_did_connect);
    });
}

static void gc_ble_swizzled_did_connect(id self, SEL _cmd, id central, id peripheral) {
    if (g_orig_did_connect) {
        ((void (*)(id, SEL, id, id))g_orig_did_connect)(self, _cmd, central, peripheral);
    }
    // The link exists now, and we are on CoreBluetooth's queue.
    apply_latency(central, peripheral);
}

static void swizzled_connect(id self, SEL _cmd, id peripheral, id options) {
    ensure_delegate_swizzled();
    if (g_orig_connect) {
        ((void (*)(id, SEL, id, id))g_orig_connect)(self, _cmd, peripheral, options);
    }
    apply_latency(self, peripheral);
}

static void swizzled_params(id self, SEL _cmd, id info) {
    if (g_orig_params) {
        ((void (*)(id, SEL, id))g_orig_params)(self, _cmd, info);
    }
    record_params(info);
}

/// Installs the hooks. Call before any `CBCentralManager` connects.
/// `level` is the latency class: 0 low (fastest) … 3 very high, or -1 to
/// observe without asking for anything.
void gc_ble_latency_install(int level) {
    g_level = level;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Class cm = objc_getClass("CBCentralManager");
        g_orig_connect = swizzle(cm, sel_registerName("connectPeripheral:options:"),
                                 (IMP)swizzled_connect);
        g_orig_params = swizzle(cm, sel_registerName("handleConnectionParametersUpdated:"),
                                (IMP)swizzled_params);
    });
}

/// True if the SPI is present on this macOS build at all.
int gc_ble_latency_supported(void) {
    Class cm = objc_getClass("CBCentralManager");
    return cm && class_getInstanceMethod(cm, set_latency_sel()) ? 1 : 0;
}

/// Which hooks are in place: bit 0 the connect hook, bit 1 the parameter
/// report, bit 2 btleplug's delegate. Bit 2 stays clear until the first
/// connect, because that class does not exist before then.
int gc_ble_latency_hooks(void) {
    return (g_orig_connect ? 1 : 0) | (g_orig_params ? 2 : 0) | (g_orig_did_connect ? 4 : 0);
}

int gc_ble_latency_applied(void) { return atomic_load(&g_applied); }
int gc_ble_latency_failed(void) { return atomic_load(&g_failed); }

/// Copies the last reported connection parameters into `out`. Returns 0 if
/// CoreBluetooth has not reported any.
int gc_ble_latency_params(char *out, int cap) {
    if (!out || cap <= 0) {
        return 0;
    }
    os_unfair_lock_lock(&g_params_lock);
    int have = g_params[0] != '\0';
    if (have) {
        strncpy(out, g_params, (size_t)cap - 1);
        out[cap - 1] = '\0';
    }
    os_unfair_lock_unlock(&g_params_lock);
    return have;
}

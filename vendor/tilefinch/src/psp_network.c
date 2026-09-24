#include "tilefinch/psp_network.h"
#include "psp_network_policy.h"
#include "psp_utility_module_contract.h"
#include "tilefinch/psp_log.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <psputility_netconf.h>
#include <psputility_msgdialog.h>
#include <psputility_netparam.h>
#include <pspwlan.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define PROFILE_QUERY_SECURITY (UINT32_C(1) << 0)
#define PROFILE_QUERY_STATIC_IP (UINT32_C(1) << 1)
#define PROFILE_QUERY_MANUAL_DNS (UINT32_C(1) << 2)
#define PROFILE_QUERY_PROXY (UINT32_C(1) << 3)

#define INTERFACE_QUERY_SECURITY (UINT32_C(1) << 0)
#define INTERFACE_QUERY_STRENGTH (UINT32_C(1) << 1)
#define INTERFACE_QUERY_CHANNEL (UINT32_C(1) << 2)
#define INTERFACE_QUERY_POWER_SAVE (UINT32_C(1) << 3)
#define INTERFACE_QUERY_PROXY (UINT32_C(1) << 4)
#define INTERFACE_QUERY_IP (UINT32_C(1) << 5)
#define INTERFACE_QUERY_SUBNET (UINT32_C(1) << 6)
#define INTERFACE_QUERY_GATEWAY (UINT32_C(1) << 7)
#define INTERFACE_QUERY_PRIMARY_DNS (UINT32_C(1) << 8)
#define INTERFACE_QUERY_SECONDARY_DNS (UINT32_C(1) << 9)

#define PSP_NETWORK_DISCONNECT_WAIT_US UINT64_C(250000)
#define PSP_NETWORK_DISCONNECT_POLL_US 5000u
#define PSP_NETWORK_PROFILE_LIMIT 100

/*
 * Name resolution is the one network primitive the application cannot bound
 * from the outside. The owned transport builds curl with
 * ENABLE_THREADED_RESOLVER=OFF, no c-ares, no DoH, no getaddrinfo and no
 * alarm(), so lib/hostip4.c takes its last branch and calls plain
 * gethostbyname(). That call runs to completion inside curl_easy_perform:
 * CURLOPT_CONNECTTIMEOUT_MS only starts counting once an address exists, and
 * the cancellation callback this browser installs is curl's progress
 * callback, which cannot fire while a resolve blocks. Whatever gethostbyname
 * costs is therefore a blind window in which the main thread cannot answer
 * Circle, cannot repaint, and cannot end a cooperate scope.
 *
 * PSPSDK's own gethostbyname is correct but is tuned for a resolver that has
 * all day. Disassembling gethostbyname.o out of libcglue.a shows it calling
 * sceNetResolverStartNtoA with a hardcoded two-second timeout and three
 * retries (`li a3,2` and `li t0,3`; psp-gcc's MIPS EABI passes the fifth
 * argument in $t0, not on the stack). An unresponsive DNS server therefore
 * costs up to four two-second rounds -- about eight seconds during which
 * nothing on this device can respond -- and the count is not configurable by
 * a caller.
 *
 * Define the symbol here instead and keep one retry: half the worst case,
 * still tolerant of a lost UDP round trip, and with the resolver scratch in
 * .bss rather than a kilobyte of a stack that is also carrying curl and Mbed
 * TLS. Our translation unit is a direct object on the link line, ahead of
 * libcglue.a, so this definition satisfies curl's reference and the archive
 * member is never pulled in -- a duplicate would have failed the link.
 * Nothing else in the program calls gethostbyname.
 *
 * The bound is deliberately transport-global; there is one resolver for the
 * process, and a faster, predictable DNS failure is the right behaviour for
 * page loads as well as for video. Resolved addresses land in curl's shared
 * DNS cache, so this cost is paid once per host rather than per request.
 */
#define PSP_DNS_TIMEOUT_SECONDS 2u
#define PSP_DNS_RETRIES 1
#define PSP_DNS_RESOLVER_BUFFER_BYTES 1024u
#define PSP_DNS_ADDRESS_FAMILY_INET 2
#define PSP_DNS_ADDRESS_BYTES 4

/* Set once psp_network_boot_connect() has started and connected the stack;
   cleared when a shutdown terminates APCTL. While set, the ladder adopts the
   running services instead of initialising them again. */
static bool psp_network_boot_services;

static uint64_t psp_network_now_us(void)
{
    return (uint64_t) sceKernelGetSystemTimeWide();
}

/* One resolver scratch buffer, one hostent, one address: gethostbyname is a
   single-result interface by definition and this browser resolves from the
   single transport worker only. Static storage keeps a kilobyte off a stack
   that is also carrying curl, mbed TLS and the caller's frames. */
static unsigned char psp_dns_scratch[PSP_DNS_RESOLVER_BUFFER_BYTES]
    __attribute__((aligned(64)));
static struct in_addr psp_dns_address;
static char *psp_dns_address_list[2];
static char *psp_dns_aliases[1];
static char psp_dns_canonical_name[256];
static struct hostent psp_dns_hostent;

struct hostent *gethostbyname(const char *name)
{
    if (name == NULL || name[0] == '\0') return NULL;
    memset(&psp_dns_address, 0, sizeof(psp_dns_address));
    if (sceNetInetInetAton(name, &psp_dns_address) == 0) {
        int resolver_id = -1;
        if (sceNetResolverCreate(
                &resolver_id, psp_dns_scratch,
                (SceSize) sizeof(psp_dns_scratch)) < 0) {
            return NULL;
        }
        int resolved = sceNetResolverStartNtoA(
            resolver_id, name, &psp_dns_address,
            PSP_DNS_TIMEOUT_SECONDS, PSP_DNS_RETRIES);
        (void) sceNetResolverDelete(resolver_id);
        if (resolved < 0) return NULL;
    }
    snprintf(psp_dns_canonical_name, sizeof(psp_dns_canonical_name),
             "%s", name);
    psp_dns_address_list[0] = (char *) &psp_dns_address;
    psp_dns_address_list[1] = NULL;
    psp_dns_aliases[0] = NULL;
    psp_dns_hostent.h_name = psp_dns_canonical_name;
    psp_dns_hostent.h_aliases = psp_dns_aliases;
    psp_dns_hostent.h_addrtype = PSP_DNS_ADDRESS_FAMILY_INET;
    psp_dns_hostent.h_length = PSP_DNS_ADDRESS_BYTES;
    psp_dns_hostent.h_addr_list = psp_dns_address_list;
    psp_dns_hostent.h_addr = psp_dns_address_list[0];
    return &psp_dns_hostent;
}

static void psp_network_sample_memory(PspNetwork *network)
{
    size_t free_memory = (size_t) sceKernelTotalFreeMemSize();
    size_t maximum_block = (size_t) sceKernelMaxFreeMemSize();
    if (network->free_memory_minimum == 0
        || free_memory < network->free_memory_minimum) {
        network->free_memory_minimum = free_memory;
    }
    if (network->maximum_free_block_minimum == 0
        || maximum_block < network->maximum_free_block_minimum) {
        network->maximum_free_block_minimum = maximum_block;
    }
}

static PspNetworkStatus psp_network_finish_pump(
    PspNetwork *network, PspNetworkStatus phase, uint64_t started_us)
{
    uint64_t now_us = psp_network_now_us();
    uint64_t pump_us = now_us >= started_us ? now_us - started_us : 0;
    network->last_pump_us = pump_us;
    if (pump_us > network->maximum_pump_us) {
        network->maximum_pump_us = pump_us;
        network->maximum_pump_phase = phase;
    }
    if (phase >= PSP_NETWORK_IDLE && phase < PSP_NETWORK_STATUS_COUNT) {
        network->phase_pump_us[phase] += pump_us;
        network->phase_pump_calls[phase]++;
    }
    network->elapsed_us = now_us >= network->started_us
        ? now_us - network->started_us : 0;
    psp_network_sample_memory(network);
    if (network->status == PSP_NETWORK_READY) {
        network->free_memory_ready =
            (size_t) sceKernelTotalFreeMemSize();
        network->maximum_free_block_ready =
            (size_t) sceKernelMaxFreeMemSize();
    }
    return network->status;
}

static void psp_network_query_profile(PspNetwork *network)
{
    static const struct {
        int parameter;
        uint32_t bit;
    } queries[] = {
        { PSP_NETPARAM_SECURE, PROFILE_QUERY_SECURITY },
        { PSP_NETPARAM_IS_STATIC_IP, PROFILE_QUERY_STATIC_IP },
        { PSP_NETPARAM_MANUAL_DNS, PROFILE_QUERY_MANUAL_DNS },
        { PSP_NETPARAM_USE_PROXY, PROFILE_QUERY_PROXY }
    };
    for (size_t index = 0; index < sizeof(queries) / sizeof(queries[0]);
         index++) {
        netData data;
        memset(&data, 0, sizeof(data));
        if (sceUtilityGetNetParam(
                network->profile_index, queries[index].parameter,
                &data) < 0) {
            network->profile_query_failure_mask |= queries[index].bit;
            continue;
        }
        network->profile_query_success_mask |= queries[index].bit;
        if (queries[index].parameter == PSP_NETPARAM_SECURE)
            network->profile_security_type = data.asUint;
        else if (queries[index].parameter == PSP_NETPARAM_IS_STATIC_IP)
            network->profile_static_ip = data.asUint != 0;
        else if (queries[index].parameter == PSP_NETPARAM_MANUAL_DNS)
            network->profile_manual_dns = data.asUint != 0;
        else if (queries[index].parameter == PSP_NETPARAM_USE_PROXY)
            network->profile_uses_proxy = data.asUint != 0;
    }
}

/* Saved PSP connection profiles are numbered from one, but deleting the
   configured/default slot can leave Tilefinch pointing at no profile at all.
   Firmware reports that case as PSP_NETPARAM_ERROR_BAD_NETCONF before the
   WLAN stack starts. On that specific error only, fall back to the first
   profile the firmware says is valid. This never changes a valid explicit
   selection and remains bounded even on corrupt profile storage. */
static int psp_network_check_profile(int profile, void *context)
{
    (void) context;
    return sceUtilityCheckNetParam(profile);
}

static bool psp_network_select_fallback_profile(PspNetwork *network)
{
    if (network == NULL) return false;
    int selected = psp_network_first_fallback_profile(
        network->requested_profile_index, PSP_NETWORK_PROFILE_LIMIT,
        psp_network_check_profile, NULL);
    if (selected == 0) return false;
    network->profile_index = selected;
    network->profile_fallback_used = true;
    return true;
}

int psp_network_choose_saved_profile(int current, int direction)
{
    return psp_network_next_saved_profile(
        current, PSP_NETWORK_PROFILE_LIMIT, direction,
        psp_network_check_profile, NULL);
}

bool psp_network_profile_is_saved(int profile)
{
    return profile >= 1 && profile <= PSP_NETWORK_PROFILE_LIMIT
        && sceUtilityCheckNetParam(profile) == 0;
}

bool psp_network_profile_ssid(
    int profile, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0u) return false;
    output[0] = '\0';
    if (!psp_network_profile_is_saved(profile)) return false;

    netData data;
    memset(&data, 0, sizeof(data));
    if (sceUtilityGetNetParam(profile, PSP_NETPARAM_SSID, &data) < 0)
        return false;
    data.asString[sizeof(data.asString) - 1u] = '\0';

    size_t written = 0u;
    for (size_t at = 0u; data.asString[at] != '\0'
         && written + 1u < output_size; at++) {
        unsigned char byte = (unsigned char) data.asString[at];
        /* Saved SSIDs are byte strings. Keep the chrome renderer boring and
           deterministic if firmware returns controls or non-UTF-8 bytes. */
        output[written++] = byte >= 0x20u && byte <= 0x7eu
            ? (char) byte : '?';
    }
    output[written] = '\0';
    return written != 0u;
}

static PspNetworkStatus psp_network_fail(PspNetwork *network, int result)
{
    network->failure_phase = network->status;
    network->native_result = result;
    uint64_t now_us = psp_network_now_us();
    network->elapsed_us = now_us >= network->started_us
        ? now_us - network->started_us : 0;
    network->status = PSP_NETWORK_FAILED;
    return network->status;
}

bool psp_network_begin(PspNetwork *network, int profile_index)
{
    if (network == NULL || profile_index <= 0) return false;
    memset(network, 0, sizeof(*network));
    network->requested_profile_index = profile_index;
    network->profile_index = profile_index;
    network->apctl_state = -1;
    network->native_result = 0;
    network->started_us = psp_network_now_us();
    network->free_memory_start =
        (size_t) sceKernelTotalFreeMemSize();
    network->free_memory_minimum = network->free_memory_start;
    network->maximum_free_block_start =
        (size_t) sceKernelMaxFreeMemSize();
    network->maximum_free_block_minimum =
        network->maximum_free_block_start;
    network->maximum_pump_phase = PSP_NETWORK_IDLE;
    network->wlan_switch_state = -1;
    network->wlan_power_state = -1;
    network->status = PSP_NETWORK_CHECKING_PROFILE;
    return true;
}

PspNetworkStatus psp_network_pump(PspNetwork *network,
                                  uint64_t timeout_us)
{
    if (network == NULL) return PSP_NETWORK_FAILED;
    if (network->status == PSP_NETWORK_READY
        || network->status == PSP_NETWORK_FAILED
        || network->status == PSP_NETWORK_CANCELLED
        || network->status == PSP_NETWORK_IDLE) return network->status;
    uint64_t now_us = psp_network_now_us();
    network->elapsed_us = now_us >= network->started_us
        ? now_us - network->started_us : 0;
    network->pump_calls++;
    PspNetworkStatus phase = network->status;
    uint64_t pump_started_us = now_us;
    if (timeout_us != 0 && network->elapsed_us >= timeout_us) {
        (void) psp_network_fail(network, -1);
        return psp_network_finish_pump(
            network, phase, pump_started_us);
    }

    int result = 0;
    switch (network->status) {
    case PSP_NETWORK_CHECKING_PROFILE:
        result = sceUtilityCheckNetParam(network->profile_index);
        if ((uint32_t) result == (uint32_t) PSP_NETPARAM_ERROR_BAD_NETCONF
            && psp_network_select_fallback_profile(network)) {
            result = 0;
        }
        /* The system dialog lists the saved connections itself and explains
           a missing one on screen, so a configured index is only a hint. */
        if (result < 0 && network->connect_via_dialog) result = 0;
        if (result < 0) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        psp_network_query_profile(network);
        network->status = PSP_NETWORK_LOADING_COMMON;
        break;
    case PSP_NETWORK_LOADING_COMMON:
        network->wlan_switch_state = sceWlanGetSwitchState();
        network->wlan_power_state = sceWlanDevIsPowerOn();
        if (psp_network_boot_services) {
            network->common_loaded = true;
            network->inet_loaded = true;
            network->core_initialized = true;
            network->inet_initialized = true;
            network->resolver_initialized = true;
            network->apctl_initialized = true;
            network->initialization_adopted_mask |= UINT32_C(0xf);
            network->status = PSP_NETWORK_CONNECTING;
            break;
        }
        /* The physical switch is a real precondition.  Device power is not:
           on a cold boot sceWlanDevIsPowerOn() remains false until the net
           modules/APCTL bring the radio up.  Rejecting that state here made
           the first connection attempt fail before the WLAN lamp could ever
           illuminate.  Keep the sample for diagnostics, then follow the
           PSPSDK connection order and let module/APCTL errors be authoritative. */
        if (network->wlan_switch_state <= 0) {
            (void) psp_network_fail(network, -2);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        PspUtilityModuleLoadDisposition common_disposition =
            psp_utility_load_net_module(
                PSP_NET_MODULE_COMMON, &result);
        if (common_disposition == PSP_UTILITY_MODULE_LOAD_FAILED) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        network->common_loaded = true;
        network->common_owned = psp_utility_module_load_owned(
            common_disposition);
        network->status = PSP_NETWORK_LOADING_INET;
        break;
    case PSP_NETWORK_LOADING_INET: {
        PspUtilityModuleLoadDisposition inet_disposition =
            psp_utility_load_net_module(
                PSP_NET_MODULE_INET, &result);
        if (inet_disposition == PSP_UTILITY_MODULE_LOAD_FAILED) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        network->inet_loaded = true;
        network->inet_owned = psp_utility_module_load_owned(
            inet_disposition);
        network->status = PSP_NETWORK_INITIALIZING_CORE;
        break;
    }
    case PSP_NETWORK_INITIALIZING_CORE:
        result = sceNetInit(0x20000, 0x2a, 0, 0x2a, 0);
        bool core_adopted = false;
        if (!psp_network_init_result_usable(
                PSP_NETWORK_INIT_CORE, result, &core_adopted)) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (core_adopted)
            network->initialization_adopted_mask |= UINT32_C(1) << 0;
        network->core_initialized = true;
        network->status = PSP_NETWORK_INITIALIZING_INET;
        break;
    case PSP_NETWORK_INITIALIZING_INET:
        result = sceNetInetInit();
        bool inet_adopted = false;
        if (!psp_network_init_result_usable(
                PSP_NETWORK_INIT_INET, result, &inet_adopted)) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (inet_adopted)
            network->initialization_adopted_mask |= UINT32_C(1) << 1;
        network->inet_initialized = true;
        network->status = PSP_NETWORK_INITIALIZING_RESOLVER;
        break;
    case PSP_NETWORK_INITIALIZING_RESOLVER:
        result = sceNetResolverInit();
        bool resolver_adopted = false;
        if (!psp_network_init_result_usable(
                PSP_NETWORK_INIT_RESOLVER, result, &resolver_adopted)) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (resolver_adopted)
            network->initialization_adopted_mask |= UINT32_C(1) << 2;
        network->resolver_initialized = true;
        network->status = PSP_NETWORK_INITIALIZING_APCTL;
        break;
    case PSP_NETWORK_INITIALIZING_APCTL:
        result = sceNetApctlInit(0x8000, 0x30);
        bool apctl_adopted = false;
        if (!psp_network_init_result_usable(
                PSP_NETWORK_INIT_APCTL, result, &apctl_adopted)) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (apctl_adopted)
            network->initialization_adopted_mask |= UINT32_C(1) << 3;
        network->apctl_initialized = true;
        network->status = PSP_NETWORK_CONNECTING;
        break;
    case PSP_NETWORK_CONNECTING:
        network->wlan_power_state = sceWlanDevIsPowerOn();
        result = sceNetApctlGetState(&network->apctl_state);
        if (result < 0) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (network->apctl_state == PSP_NET_APCTL_STATE_GOT_IP) {
            network->status = PSP_NETWORK_READY;
            break;
        }
        if (psp_network_apctl_state_is_associating(
                network->apctl_state)) {
            /* An APCTL service adopted after an incomplete teardown can
               still be finishing its prior association. Reissuing Connect
               in JOINING/GETTING_IP/EAP/KEY_EXCHANGE is rejected by real
               firmware; observe the existing transaction instead. */
            network->connect_started = true;
            network->status = PSP_NETWORK_WAITING_FOR_IP;
            break;
        }
        if (network->connect_via_dialog) {
            /* Association belongs to the firmware dialog, which must run
               on the owner's thread with the GE to itself. */
            network->dialog_pending = true;
            break;
        }
        result = sceNetApctlConnect(network->profile_index);
        if (result < 0) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        network->connect_started = true;
        network->status = PSP_NETWORK_WAITING_FOR_IP;
        break;
    case PSP_NETWORK_WAITING_FOR_IP:
        network->wlan_power_state = sceWlanDevIsPowerOn();
        result = sceNetApctlGetState(&network->apctl_state);
        if (result < 0) {
            (void) psp_network_fail(network, result);
            return psp_network_finish_pump(
                network, phase, pump_started_us);
        }
        if (network->apctl_state == PSP_NET_APCTL_STATE_GOT_IP)
            network->status = PSP_NETWORK_READY;
        break;
    default:
        (void) psp_network_fail(network, -3);
        return psp_network_finish_pump(
            network, phase, pump_started_us);
    }
    network->native_result = result;
    return psp_network_finish_pump(network, phase, pump_started_us);
}

#define PSP_NETCONF_SCREEN_WIDTH 480
#define PSP_NETCONF_SCREEN_HEIGHT 272
#define PSP_NETCONF_VRAM_STRIDE 512

static unsigned int psp_netconf_gu_list[1024] __attribute__((aligned(16)));

static void psp_system_dialog_fill_base(pspUtilityDialogCommon *base,
                                        unsigned size)
{
    memset(base, 0, sizeof(*base));
    base->size = size;
    sceUtilityGetSystemParamInt(
        PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &base->language);
    /* Firmware setting 9 is the X/O accept-button choice. */
    sceUtilityGetSystemParamInt(
        PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &base->buttonSwap);
    base->graphicsThread = 17;
    base->accessThread = 19;
    base->fontThread = 18;
    base->soundThread = 16;
}

/* Drive one system utility dialog to completion. The dialog draws through
   the GE into the two scanout buffers, so the caller must repaint its own
   surface afterwards. */
static void psp_system_dialog_run(int (*get_status)(void),
                                  void (*update)(void),
                                  void (*shutdown)(void))
{
    const int buffer_bytes = PSP_NETCONF_VRAM_STRIDE
        * PSP_NETCONF_SCREEN_HEIGHT * (int) sizeof(uint16_t);
    sceGuInit();
    sceGuStart(GU_DIRECT, psp_netconf_gu_list);
    sceGuDrawBuffer(GU_PSM_5650, (void *) (uintptr_t) buffer_bytes,
                    PSP_NETCONF_VRAM_STRIDE);
    sceGuDispBuffer(PSP_NETCONF_SCREEN_WIDTH, PSP_NETCONF_SCREEN_HEIGHT,
                    (void *) 0, PSP_NETCONF_VRAM_STRIDE);
    sceGuOffset(2048 - PSP_NETCONF_SCREEN_WIDTH / 2,
                2048 - PSP_NETCONF_SCREEN_HEIGHT / 2);
    sceGuViewport(2048, 2048, PSP_NETCONF_SCREEN_WIDTH,
                  PSP_NETCONF_SCREEN_HEIGHT);
    sceGuScissor(0, 0, PSP_NETCONF_SCREEN_WIDTH, PSP_NETCONF_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    bool shutdown_requested = false;
    for (;;) {
        psp_log_heartbeat();
        sceGuStart(GU_DIRECT, psp_netconf_gu_list);
        sceGuClearColor(0x000f0f0fu);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        int status = get_status();
        if (status == PSP_UTILITY_DIALOG_NONE) break;
        if (status == PSP_UTILITY_DIALOG_VISIBLE) {
            update();
        } else if (status == PSP_UTILITY_DIALOG_QUIT && !shutdown_requested) {
            shutdown();
            shutdown_requested = true;
        }
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
}

static void psp_netconf_update(void) { (void) sceUtilityNetconfUpdate(1); }
static void psp_netconf_shutdown(void)
{
    (void) sceUtilityNetconfShutdownStart();
}
static void psp_msgdialog_update(void) { sceUtilityMsgDialogUpdate(1); }

/* Firmware netconf action 3: join the access point used last, showing
   only the progress dialog. PSPSDK names actions 0..2 only. */
#define PSP_NETCONF_ACTION_CONNECTAP_LASTUSED 3

static int psp_network_run_netconf(int action)
{
    struct pspUtilityNetconfAdhoc adhoc;
    memset(&adhoc, 0, sizeof(adhoc));
    pspUtilityNetconfData data;
    memset(&data, 0, sizeof(data));
    psp_system_dialog_fill_base(&data.base, sizeof(data));
    data.action = action;
    data.adhocparam = &adhoc;
    /* Accept profiles saved with "Internet Browser: Start" as well. */
    data.hotspot = 1;
    int started = sceUtilityNetconfInitStart(&data);
    printf("tilefinch-network-dialog: init=0x%08x\n", (unsigned) started);
    if (started < 0) return started;
    psp_system_dialog_run(sceUtilityNetconfGetStatus, psp_netconf_update,
                          psp_netconf_shutdown);
    printf("tilefinch-network-dialog: action=%d result=%d\n",
           action, data.base.result);
    return data.base.result;
}

int psp_network_run_connect_dialog(void)
{
    return psp_network_run_netconf(PSP_NETCONF_ACTION_CONNECTAP);
}

/* Show a firmware message box. error_code != 0 shows the firmware's own
   explanation of that error code. Returns true when the user chose Yes. */
static bool psp_network_message(const char *text, unsigned error_code,
                                bool yes_no)
{
    pspUtilityMsgDialogParams params;
    memset(&params, 0, sizeof(params));
    psp_system_dialog_fill_base(&params.base, sizeof(params));
    if (error_code != 0) {
        params.mode = PSP_UTILITY_MSGDIALOG_MODE_ERROR;
        params.options = PSP_UTILITY_MSGDIALOG_OPTION_ERROR;
        params.errorValue = error_code;
    } else {
        params.mode = PSP_UTILITY_MSGDIALOG_MODE_TEXT;
        params.options = PSP_UTILITY_MSGDIALOG_OPTION_TEXT
            | (yes_no ? PSP_UTILITY_MSGDIALOG_OPTION_YESNO_BUTTONS : 0);
        snprintf(params.message, sizeof(params.message), "%s", text);
    }
    if (sceUtilityMsgDialogInitStart(&params) < 0) return false;
    psp_system_dialog_run(sceUtilityMsgDialogGetStatus, psp_msgdialog_update,
                          sceUtilityMsgDialogShutdownStart);
    return params.buttonPressed == PSP_UTILITY_MSGDIALOG_RESULT_YES;
}

static int psp_network_connected_profile(int fallback);

PspBootConnectResult psp_network_boot_connect(int *profile_out)
{
    int result = sceWlanGetSwitchState();
    printf("tilefinch-boot-wifi: wlan-switch=%d\n", result);
    int status = 0;
    if (psp_utility_load_net_module(PSP_NET_MODULE_COMMON, &status)
            == PSP_UTILITY_MODULE_LOAD_FAILED
        || psp_utility_load_net_module(PSP_NET_MODULE_INET, &status)
            == PSP_UTILITY_MODULE_LOAD_FAILED) {
        printf("tilefinch-boot-wifi: module-load=0x%08x\n", (unsigned) status);
        (void) psp_network_message(NULL, (unsigned) status, false);
        return PSP_BOOT_CONNECT_FAILED;
    }
    /* The PSPSDK / firmware-sample parameters. The browser's own ladder later
       adopts these services through the documented "already initialised"
       statuses instead of initialising a second time. */
    static const PspNetworkInitService services[] = {
        PSP_NETWORK_INIT_CORE, PSP_NETWORK_INIT_INET,
        PSP_NETWORK_INIT_RESOLVER, PSP_NETWORK_INIT_APCTL
    };
    for (size_t at = 0; at < sizeof(services) / sizeof(services[0]); at++) {
        switch (services[at]) {
        case PSP_NETWORK_INIT_CORE:
            status = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
            break;
        case PSP_NETWORK_INIT_INET: status = sceNetInetInit(); break;
        case PSP_NETWORK_INIT_RESOLVER: status = sceNetResolverInit(); break;
        case PSP_NETWORK_INIT_APCTL:
            status = sceNetApctlInit(0x8000, 0x30);
            break;
        }
        if (!psp_network_init_result_usable(services[at], status, NULL)) {
            printf("tilefinch-boot-wifi: init[%u]=0x%08x\n",
                   (unsigned) at, (unsigned) status);
            (void) psp_network_message(NULL, (unsigned) status, false);
            return PSP_BOOT_CONNECT_FAILED;
        }
    }
    /* Try the last-used access point first so a normal start needs no
       button press; the chooser only appears if that does not connect. */
    bool try_last_used = true;
    for (;;) {
        int state = -1;
        if (sceNetApctlGetState(&state) >= 0
            && state == PSP_NET_APCTL_STATE_GOT_IP) break;
        int dialog;
        if (try_last_used) {
            try_last_used = false;
            dialog = psp_network_run_netconf(
                PSP_NETCONF_ACTION_CONNECTAP_LASTUSED);
            state = -1;
            (void) sceNetApctlGetState(&state);
            printf("tilefinch-boot-wifi: last-used=%d apctl=%d\n",
                   dialog, state);
            if (state == PSP_NET_APCTL_STATE_GOT_IP) break;
            (void) sceNetApctlDisconnect();
        }
        dialog = psp_network_run_connect_dialog();
        state = -1;
        (void) sceNetApctlGetState(&state);
        printf("tilefinch-boot-wifi: dialog=%d apctl=%d\n", dialog, state);
        if (state == PSP_NET_APCTL_STATE_GOT_IP) break;
        if (dialog < 0) {
            (void) psp_network_message(NULL, (unsigned) dialog, false);
            return PSP_BOOT_CONNECT_FAILED;
        }
        if (!psp_network_message(
                "Not connected to Wi-Fi.\n\n"
                "komi-tube needs an Internet connection.\n"
                "Open the Wi-Fi connection screen again?\n"
                "(No quits the app.)", 0, true))
            return PSP_BOOT_CONNECT_DECLINED;
    }
    psp_network_boot_services = true;
    int profile = psp_network_connected_profile(0);
    if (profile_out != NULL) *profile_out = profile;
    printf("tilefinch-boot-wifi: connected profile=%d\n", profile);
    return PSP_BOOT_CONNECT_READY;
}

/* Recover the saved-profile index for the connection the dialog joined, so
   rejoin after sleep reconnects to the same access point. */
static int psp_network_connected_profile(int fallback)
{
    union SceNetApctlInfo info;
    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PROFILE_NAME, &info) < 0)
        return fallback;
    info.name[sizeof(info.name) - 1u] = '\0';
    if (info.name[0] == '\0') return fallback;
    for (int profile = 1; profile <= PSP_NETWORK_PROFILE_LIMIT; profile++) {
        if (sceUtilityCheckNetParam(profile) != 0) continue;
        netData data;
        memset(&data, 0, sizeof(data));
        if (sceUtilityGetNetParam(profile, PSP_NETPARAM_NAME, &data) < 0)
            continue;
        data.asString[sizeof(data.asString) - 1u] = '\0';
        if (strcmp(data.asString, info.name) == 0) return profile;
    }
    return fallback;
}

PspNetworkStatus psp_network_finish_dialog(PspNetwork *network,
                                           int dialog_result)
{
    if (network == NULL) return PSP_NETWORK_FAILED;
    network->dialog_pending = false;
    int state = -1;
    int result = sceNetApctlGetState(&state);
    network->apctl_state = state;
    network->wlan_power_state = sceWlanDevIsPowerOn();
    uint64_t now_us = psp_network_now_us();
    network->elapsed_us = now_us >= network->started_us
        ? now_us - network->started_us : 0;
    psp_network_sample_memory(network);
    if (result >= 0 && state == PSP_NET_APCTL_STATE_GOT_IP) {
        network->connect_started = true;
        network->profile_index =
            psp_network_connected_profile(network->profile_index);
        network->native_result = 0;
        network->status = PSP_NETWORK_READY;
        network->free_memory_ready = (size_t) sceKernelTotalFreeMemSize();
        network->maximum_free_block_ready =
            (size_t) sceKernelMaxFreeMemSize();
        return network->status;
    }
    network->failure_phase = PSP_NETWORK_CONNECTING;
    network->native_result = dialog_result < 0
        ? dialog_result : (result < 0 ? result : dialog_result);
    network->status = dialog_result == 1
        ? PSP_NETWORK_CANCELLED : PSP_NETWORK_FAILED;
    return network->status;
}

void psp_network_mark_cancelled(PspNetwork *network)
{
    if (network == NULL || network->status == PSP_NETWORK_READY
        || network->status == PSP_NETWORK_FAILED
        || network->status == PSP_NETWORK_CANCELLED) return;
    uint64_t now_us = psp_network_now_us();
    network->elapsed_us = now_us >= network->started_us
        ? now_us - network->started_us : 0;
    network->status = PSP_NETWORK_CANCELLED;
}

static void psp_network_query_interface_field(
    PspNetworkInterfaceReport *report, int code, uint32_t bit,
    union SceNetApctlInfo *info)
{
    memset(info, 0, sizeof(*info));
    int result = sceNetApctlGetInfo(code, info);
    if (result < 0) {
        report->query_failure_mask |= bit;
        if (report->first_error == 0) report->first_error = result;
    } else {
        report->query_success_mask |= bit;
    }
}

bool psp_network_interface_report(
    const PspNetwork *network, PspNetworkInterfaceReport *report)
{
    if (network == NULL || report == NULL
        || network->status != PSP_NETWORK_READY) return false;
    memset(report, 0, sizeof(*report));
    union SceNetApctlInfo info;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_SECURITY_TYPE,
        INTERFACE_QUERY_SECURITY, &info);
    report->security_type = info.securityType;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_STRENGTH,
        INTERFACE_QUERY_STRENGTH, &info);
    report->signal_strength = info.strength;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_CHANNEL,
        INTERFACE_QUERY_CHANNEL, &info);
    report->channel = info.channel;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_POWER_SAVE,
        INTERFACE_QUERY_POWER_SAVE, &info);
    report->power_save = info.powerSave != 0;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_USE_PROXY,
        INTERFACE_QUERY_PROXY, &info);
    report->uses_proxy = info.useProxy != 0;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_IP, INTERFACE_QUERY_IP, &info);
    report->has_ip = info.ip[0] != '\0'
        && strcmp(info.ip, "0.0.0.0") != 0;
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_SUBNETMASK,
        INTERFACE_QUERY_SUBNET, &info);
    report->has_subnet = info.subNetMask[0] != '\0';
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_GATEWAY,
        INTERFACE_QUERY_GATEWAY, &info);
    report->has_gateway = info.gateway[0] != '\0';
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_PRIMDNS,
        INTERFACE_QUERY_PRIMARY_DNS, &info);
    report->has_primary_dns = info.primaryDns[0] != '\0';
    psp_network_query_interface_field(
        report, PSP_NET_APCTL_INFO_SECDNS,
        INTERFACE_QUERY_SECONDARY_DNS, &info);
    report->has_secondary_dns = info.secondaryDns[0] != '\0';
    /* Some valid profiles (and PPSSPP's emulated AP) expose only the
       secondary resolver. Either resolver is sufficient for live traffic and
       for the chrome's WLAN-strength indicator. */
    return report->has_ip
        && (report->has_primary_dns || report->has_secondary_dns);
}

bool psp_network_resume_ready(
    PspNetwork *network, int *native_result)
{
    if (native_result != NULL) *native_result = 0;
    if (network == NULL || network->status != PSP_NETWORK_READY
        || !network->apctl_initialized) return false;
    int state = -1;
    int result = sceNetApctlGetState(&state);
    if (native_result != NULL) *native_result = result;
    network->apctl_state = state;
    network->wlan_switch_state = sceWlanGetSwitchState();
    network->wlan_power_state = sceWlanDevIsPowerOn();
    if (result < 0 || state != PSP_NET_APCTL_STATE_GOT_IP
        || network->wlan_switch_state <= 0
        || network->wlan_power_state <= 0) {
        return false;
    }
    PspNetworkInterfaceReport report;
    return psp_network_interface_report(network, &report);
}

bool psp_network_link_ready(PspNetwork *network, int *native_result)
{
    if (native_result != NULL) *native_result = 0;
    if (network == NULL || !network->apctl_initialized) return false;
    int state = -1;
    int result = sceNetApctlGetState(&state);
    if (native_result != NULL) *native_result = result;
    network->apctl_state = state;
    network->wlan_switch_state = sceWlanGetSwitchState();
    network->wlan_power_state = sceWlanDevIsPowerOn();
    return result >= 0 && state == PSP_NET_APCTL_STATE_GOT_IP
        && network->wlan_switch_state > 0
        && network->wlan_power_state > 0;
}

void psp_network_rejoin_begin(
    PspNetwork *network, PspNetworkRejoinOperation *operation)
{
    if (operation == NULL) return;
    memset(operation, 0, sizeof(*operation));
    if (network == NULL || !network->apctl_initialized
        || network->profile_index <= 0) {
        operation->phase = PSP_NETWORK_REJOIN_FAILED;
        operation->native_result = -1;
        return;
    }
    operation->started_us = psp_network_now_us();
    operation->phase_started_us = operation->started_us;
    operation->phase = PSP_NETWORK_REJOIN_LEAVING;
    network->disconnect_started = false;
    network->connect_started = false;
}

bool psp_network_rejoin_pump(
    PspNetwork *network, PspNetworkRejoinOperation *operation,
    uint64_t timeout_us)
{
    if (network == NULL || operation == NULL
        || operation->phase == PSP_NETWORK_REJOIN_COMPLETE
        || operation->phase == PSP_NETWORK_REJOIN_FAILED)
        return true;
    uint64_t now_us = psp_network_now_us();
    if (timeout_us != 0
        && now_us - operation->started_us >= timeout_us) {
        operation->phase = PSP_NETWORK_REJOIN_FAILED;
        operation->native_result = -1;
        return true;
    }
    int state = PSP_NET_APCTL_STATE_DISCONNECTED;
    int result = sceNetApctlGetState(&state);
    operation->native_result = result;
    if (result < 0) {
        operation->phase = PSP_NETWORK_REJOIN_FAILED;
        return true;
    }
    network->apctl_state = state;
    if (operation->phase == PSP_NETWORK_REJOIN_LEAVING) {
        if (state != PSP_NET_APCTL_STATE_DISCONNECTED) {
            if (!network->disconnect_started) {
                result = sceNetApctlDisconnect();
                operation->native_result = result;
                if (result < 0) {
                    operation->phase = PSP_NETWORK_REJOIN_FAILED;
                    return true;
                }
                network->disconnect_started = true;
            } else if (now_us - operation->phase_started_us
                       >= PSP_NETWORK_DISCONNECT_WAIT_US) {
                operation->phase = PSP_NETWORK_REJOIN_FAILED;
                operation->native_result = -2;
                return true;
            }
            return false;
        }
        result = sceNetApctlConnect(network->profile_index);
        operation->native_result = result;
        if (result < 0) {
            operation->phase = PSP_NETWORK_REJOIN_FAILED;
            return true;
        }
        network->disconnect_started = false;
        network->connect_started = true;
        operation->phase = PSP_NETWORK_REJOIN_WAITING_IP;
        operation->phase_started_us = now_us;
        return false;
    }
    if (state == PSP_NET_APCTL_STATE_GOT_IP) {
        network->status = PSP_NETWORK_READY;
        operation->phase = PSP_NETWORK_REJOIN_COMPLETE;
        return true;
    }
    return false;
}

static void psp_network_shutdown_result(
    PspNetworkShutdownReport *report, uint32_t bit, int result)
{
    report->attempted_mask |= bit;
    if (result < 0) {
        report->failure_mask |= bit;
        if (report->first_error == 0) report->first_error = result;
    }
}

void psp_network_shutdown_begin(
    PspNetwork *network, PspNetworkShutdownOperation *operation)
{
    if (operation == NULL) return;
    memset(operation, 0, sizeof(*operation));
    if (network == NULL) {
        operation->phase = PSP_NETWORK_SHUTDOWN_COMPLETE;
        return;
    }
    operation->started_us = psp_network_now_us();
    operation->leave_started_us = operation->started_us;
    operation->report.final_apctl_state = network->apctl_state;
    operation->report.free_memory_before =
        (size_t) sceKernelTotalFreeMemSize();
    operation->report.maximum_free_block_before =
        (size_t) sceKernelMaxFreeMemSize();
    operation->phase = network->apctl_initialized
        ? PSP_NETWORK_SHUTDOWN_LEAVING
        : PSP_NETWORK_SHUTDOWN_TERM_RESOLVER;
}

bool psp_network_shutdown_pump(
    PspNetwork *network, PspNetworkShutdownOperation *operation)
{
    if (network == NULL || operation == NULL
        || operation->phase == PSP_NETWORK_SHUTDOWN_COMPLETE)
        return true;
    PspNetworkShutdownReport *report = &operation->report;
    switch (operation->phase) {
    case PSP_NETWORK_SHUTDOWN_LEAVING: {
        int state = PSP_NET_APCTL_STATE_DISCONNECTED;
        int queried = sceNetApctlGetState(&state);
        report->disconnect_polls++;
        if (queried < 0) {
            psp_network_shutdown_result(
                report, UINT32_C(1) << 7, queried);
            state = network->apctl_state;
            operation->leave_timed_out = true;
            operation->phase = PSP_NETWORK_SHUTDOWN_TERM_APCTL;
        } else {
            network->apctl_state = state;
            report->final_apctl_state = state;
            if (state == PSP_NET_APCTL_STATE_DISCONNECTED) {
                operation->phase = PSP_NETWORK_SHUTDOWN_TERM_APCTL;
            } else if (!network->disconnect_started) {
                int disconnected = sceNetApctlDisconnect();
                psp_network_shutdown_result(
                    report, UINT32_C(1) << 0, disconnected);
                if (disconnected >= 0)
                    network->disconnect_started = true;
                else {
                    operation->leave_timed_out = true;
                    operation->phase = PSP_NETWORK_SHUTDOWN_TERM_APCTL;
                }
            } else if (psp_network_now_us() - operation->leave_started_us
                       >= PSP_NETWORK_DISCONNECT_WAIT_US) {
                operation->leave_timed_out = true;
                operation->phase = PSP_NETWORK_SHUTDOWN_TERM_APCTL;
            }
        }
        report->disconnect_wait_us =
            psp_network_now_us() - operation->leave_started_us;
        return false;
    }
    case PSP_NETWORK_SHUTDOWN_TERM_APCTL:
        psp_network_boot_services = false;
        if (network->apctl_initialized)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 1, sceNetApctlTerm());
        network->apctl_initialized = false;
        network->connect_started = false;
        network->disconnect_started = false;
        operation->phase = PSP_NETWORK_SHUTDOWN_TERM_RESOLVER;
        return false;
    case PSP_NETWORK_SHUTDOWN_TERM_RESOLVER:
        if (network->resolver_initialized)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 2, sceNetResolverTerm());
        network->resolver_initialized = false;
        operation->phase = PSP_NETWORK_SHUTDOWN_TERM_INET;
        return false;
    case PSP_NETWORK_SHUTDOWN_TERM_INET:
        if (network->inet_initialized)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 3, sceNetInetTerm());
        network->inet_initialized = false;
        operation->phase = PSP_NETWORK_SHUTDOWN_TERM_CORE;
        return false;
    case PSP_NETWORK_SHUTDOWN_TERM_CORE:
        if (network->core_initialized)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 4, sceNetTerm());
        network->core_initialized = false;
        operation->phase = PSP_NETWORK_SHUTDOWN_UNLOAD_INET;
        return false;
    case PSP_NETWORK_SHUTDOWN_UNLOAD_INET:
        if (network->inet_owned)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 5,
                sceUtilityUnloadNetModule(PSP_NET_MODULE_INET));
        network->inet_owned = false;
        network->inet_loaded = false;
        operation->phase = PSP_NETWORK_SHUTDOWN_UNLOAD_COMMON;
        return false;
    case PSP_NETWORK_SHUTDOWN_UNLOAD_COMMON:
        if (network->common_owned)
            psp_network_shutdown_result(
                report, UINT32_C(1) << 6,
                sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON));
        network->common_owned = false;
        network->common_loaded = false;
        report->elapsed_us = psp_network_now_us() - operation->started_us;
        report->free_memory_after =
            (size_t) sceKernelTotalFreeMemSize();
        report->maximum_free_block_after =
            (size_t) sceKernelMaxFreeMemSize();
        memset(network, 0, sizeof(*network));
        operation->phase = PSP_NETWORK_SHUTDOWN_COMPLETE;
        return true;
    case PSP_NETWORK_SHUTDOWN_IDLE:
    case PSP_NETWORK_SHUTDOWN_COMPLETE:
    default:
        return true;
    }
}

void psp_network_shutdown(
    PspNetwork *network, PspNetworkShutdownReport *report)
{
    if (network == NULL) return;
    PspNetworkShutdownOperation operation;
    psp_network_shutdown_begin(network, &operation);
    while (!psp_network_shutdown_pump(network, &operation)) {
        if (operation.phase == PSP_NETWORK_SHUTDOWN_LEAVING)
            (void) sceKernelDelayThread(PSP_NETWORK_DISCONNECT_POLL_US);
    }
    if (report != NULL) *report = operation.report;
}

const char *psp_network_status_name(PspNetworkStatus status)
{
    switch (status) {
    case PSP_NETWORK_IDLE: return "idle";
    case PSP_NETWORK_CHECKING_PROFILE: return "check-profile";
    case PSP_NETWORK_LOADING_COMMON: return "load-common";
    case PSP_NETWORK_LOADING_INET: return "load-inet";
    case PSP_NETWORK_INITIALIZING_CORE: return "init-core";
    case PSP_NETWORK_INITIALIZING_INET: return "init-inet";
    case PSP_NETWORK_INITIALIZING_RESOLVER: return "init-resolver";
    case PSP_NETWORK_INITIALIZING_APCTL: return "init-apctl";
    case PSP_NETWORK_CONNECTING: return "connect";
    case PSP_NETWORK_WAITING_FOR_IP: return "waiting-ip";
    case PSP_NETWORK_READY: return "ready";
    case PSP_NETWORK_FAILED: return "failed";
    case PSP_NETWORK_CANCELLED: return "cancelled";
    case PSP_NETWORK_STATUS_COUNT: return "count";
    default: return "unknown";
    }
}

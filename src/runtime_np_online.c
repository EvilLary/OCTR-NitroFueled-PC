#include "runtime_np_online.h"
#include "runtime_net.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORBIS_OK 0
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)

#pragma pack(push, 1)
typedef struct {
    char data[16];
    int8_t term;
    int8_t dummy[3];
} OrbisNpOnlineId;

typedef struct {
    OrbisNpOnlineId handle;
    uint8_t opt[8];
    uint8_t reserved[8];
} OrbisNpId;

typedef struct {
    char token[4097];
    uint8_t padding[7];
} OrbisNpIdToken;

typedef struct {
    char code[129];
    uint8_t padding[7];
} OrbisNpAuthCode;
#pragma pack(pop)

typedef ABI void (*NpStateCallbackLegacy)(int32_t user_id, int32_t state, const void *np_id, void *arg);
typedef ABI void (*NpStateCallbackToolkit)(int32_t user_id, int32_t state, void *arg);

static struct {
    NpStateCallbackLegacy legacy_cb;
    void *legacy_arg;
    int legacy_pending;

    NpStateCallbackToolkit toolkit_cb;
    void *toolkit_arg;
    int toolkit_pending;
} g_np_state;

static const char *get_online_name(void) {
    const char *name = getenv("BB_ONLINE_NAME");
    if (!name || !*name) name = getenv("BB_USER_NAME");
    if (!name || !*name) name = "RyuNET";
    return name;
}

const char *runtime_np_get_online_name(void) {
    return get_online_name();
}

static inline uint32_t ror32_online(uint32_t val, int shift) {
    return (val >> shift) | (val << (32 - shift));
}

static void sha256_online(const uint8_t *data, size_t len, uint8_t hash[32]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t H[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t buf[64];
    size_t padded_len = ((len + 8 + 64) / 64) * 64;
    for (size_t chunk = 0; chunk < padded_len; chunk += 64) {
        for (int i = 0; i < 64; i++) {
            size_t idx = chunk + i;
            if (idx < len) buf[i] = data[idx];
            else if (idx == len) buf[i] = 0x80;
            else if (idx >= padded_len - 8) {
                int shift = (7 - (int)(idx - (padded_len - 8))) * 8;
                buf[i] = (uint8_t)(((uint64_t)len * 8) >> shift);
            } else buf[i] = 0;
        }
        uint32_t W[64];
        for (int t = 0; t < 16; t++) {
            W[t] = ((uint32_t)buf[t*4] << 24) | ((uint32_t)buf[t*4+1] << 16) |
                   ((uint32_t)buf[t*4+2] << 8) | ((uint32_t)buf[t*4+3]);
        }
        for (int t = 16; t < 64; t++) {
            uint32_t s0 = ror32_online(W[t-15], 7) ^ ror32_online(W[t-15], 18) ^ (W[t-15] >> 3);
            uint32_t s1 = ror32_online(W[t-2], 17) ^ ror32_online(W[t-2], 19) ^ (W[t-2] >> 10);
            W[t] = W[t-16] + s0 + W[t-7] + s1;
        }
        uint32_t a=H[0], b=H[1], c=H[2], d=H[3], e=H[4], f=H[5], g=H[6], h=H[7];
        for (int t = 0; t < 64; t++) {
            uint32_t S1 = ror32_online(e, 6) ^ ror32_online(e, 11) ^ ror32_online(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + S1 + ch + K[t] + W[t];
            uint32_t S0 = ror32_online(a, 2) ^ ror32_online(a, 13) ^ ror32_online(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        H[0] += a; H[1] += b; H[2] += c; H[3] += d;
        H[4] += e; H[5] += f; H[6] += g; H[7] += h;
    }
    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (uint8_t)(H[i] >> 24);
        hash[i*4+1] = (uint8_t)(H[i] >> 16);
        hash[i*4+2] = (uint8_t)(H[i] >> 8);
        hash[i*4+3] = (uint8_t)(H[i]);
    }
}

uint64_t runtime_np_calculate_account_id(const char *name) {
    if (!name || !*name) return 0;
    uint64_t h = 14695981039346656037ULL;
    char lower[128];
    size_t i = 0;
    for (; name[i] && i < sizeof(lower) - 1; i++) {
        char c = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] + 32) : name[i];
        lower[i] = c;
        h ^= (uint8_t)c;
        h *= 1099511628211ULL;
    }
    lower[i] = '\0';
    uint8_t sha[32];
    sha256_online((const uint8_t *)lower, i, sha);
    uint64_t sha_u64 = 0;
    memcpy(&sha_u64, sha, sizeof(sha_u64));
    uint64_t mix = sha_u64 ^ h;
    uint64_t pid = 1810000000ULL + (mix % 80000000ULL);
    if (pid == 0) pid = 1810000001ULL;
    return pid;
}

static uint64_t get_online_account_id(void) {
    const char *env_acc = getenv("BB_ONLINE_ACCOUNT_ID");
    if (env_acc && *env_acc) {
        char *end = NULL;
        unsigned long long val = strtoull(env_acc, &end, 10);
        if (val > 0) return (uint64_t)val;
    }
    const char *name = get_online_name();
    return runtime_np_calculate_account_id(name);
}

static char s_friends_csv[1024] = {0};
static int s_friends_loaded = 0;

static void load_friends_if_needed(void) {
    if (s_friends_loaded) return;
    s_friends_loaded = 1;
    const char *env_f = getenv("BB_ONLINE_FRIENDS");
    if (env_f && *env_f) {
        strncpy(s_friends_csv, env_f, sizeof(s_friends_csv) - 1);
        s_friends_csv[sizeof(s_friends_csv) - 1] = '\0';
        return;
    }
    const char *cfg = getenv("BB_CONFIG");
    if (!cfg || !*cfg) cfg = "bbport.ini";
    FILE *f = fopen(cfg, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "online_friends", 14) == 0) {
                p += 14;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '=') {
                    p++;
                    while (*p == ' ' || *p == '\t') p++;
                    char *end = p + strlen(p);
                    while (end > p && (*(end - 1) == '\r' || *(end - 1) == '\n' || *(end - 1) == ' ' || *(end - 1) == '\t')) {
                        end--;
                    }
                    *end = '\0';
                    strncpy(s_friends_csv, p, sizeof(s_friends_csv) - 1);
                    s_friends_csv[sizeof(s_friends_csv) - 1] = '\0';
                    break;
                }
            }
        }
        fclose(f);
    }
}

const char *runtime_np_get_friends_csv(void) {
    load_friends_if_needed();
    return s_friends_csv;
}

int32_t runtime_np_get_state(int32_t user, int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    (void)user;
    if (!runtime_net_is_online()) {
        *state = ORBIS_NP_STATE_SIGNED_OUT;
        runtime_net_trace("sceNpGetState(user=%d) -> SIGNED_OUT", user);
        return ORBIS_OK;
    }
    *state = ORBIS_NP_STATE_SIGNED_IN;
    runtime_net_trace("sceNpGetState(user=%d) -> SIGNED_IN", user);
    return ORBIS_OK;
}

int32_t runtime_np_get_online_id(int32_t user, void *online_id_buf) {
    if (!online_id_buf) return NP_INVALID_ARGUMENT;
    (void)user;
    if (!runtime_net_is_online()) {
        runtime_net_trace("sceNpGetOnlineId -> NP_SIGNED_OUT");
        return NP_SIGNED_OUT;
    }
    OrbisNpOnlineId *id = (OrbisNpOnlineId *)online_id_buf;
    memset(id, 0, sizeof(*id));
    const char *name = get_online_name();
    strncpy(id->data, name, sizeof(id->data) - 1);
    id->term = 0;
    runtime_net_trace("sceNpGetOnlineId -> '%s'", id->data);
    return ORBIS_OK;
}

int32_t runtime_np_get_np_id(int32_t user, void *np_id_buf) {
    if (!np_id_buf) return NP_INVALID_ARGUMENT;
    (void)user;
    if (!runtime_net_is_online()) {
        runtime_net_trace("sceNpGetNpId -> NP_SIGNED_OUT");
        return NP_SIGNED_OUT;
    }
    OrbisNpId *id = (OrbisNpId *)np_id_buf;
    memset(id, 0, sizeof(*id));
    runtime_np_get_online_id(user, &id->handle);
    runtime_net_trace("sceNpGetNpId -> '%s'", id->handle.data);
    return ORBIS_OK;
}

int32_t runtime_np_get_account_id(int32_t user, uint64_t *account_id) {
    if (!account_id) return NP_INVALID_ARGUMENT;
    (void)user;
    if (!runtime_net_is_online()) {
        runtime_net_trace("sceNpGetAccountIdA -> NP_SIGNED_OUT");
        return NP_SIGNED_OUT;
    }
    *account_id = get_online_account_id();
    runtime_net_trace("sceNpGetAccountIdA -> %llu", (unsigned long long)*account_id);
    return ORBIS_OK;
}

int32_t runtime_np_get_user_id_by_account_id(uint64_t account_id, int32_t *user_id) {
    if (!user_id) return NP_INVALID_ARGUMENT;
    if (!runtime_net_is_online()) {
        runtime_net_trace("sceNpGetUserIdByAccountId -> NP_SIGNED_OUT");
        return NP_SIGNED_OUT;
    }
    if (account_id == get_online_account_id()) {
        *user_id = 1;
        runtime_net_trace("sceNpGetUserIdByAccountId(acc=%llu) -> user_id=1 (local player)", (unsigned long long)account_id);
        return ORBIS_OK;
    }
    *user_id = -1;
    runtime_net_trace("sceNpGetUserIdByAccountId(acc=%llu) -> remote friend (USER_NOT_FOUND)", (unsigned long long)account_id);
    return (int32_t)0x80550005; /* SCE_NP_ERROR_USER_NOT_FOUND */
}

int32_t runtime_np_check_plus(int32_t req_id, const void *param, void *result_buf) {
    (void)req_id; (void)param;
    if (result_buf) {
        memset(result_buf, 0, 33);
        *(uint8_t *)result_buf = 1; /* authorized = true */
    }
    if (!runtime_net_is_online()) {
        runtime_net_trace("sceNpCheckPlus -> NP_SIGNED_OUT");
        return NP_SIGNED_OUT;
    }
    runtime_net_trace("sceNpCheckPlus -> OK (PS Plus authorized=true)");
    return ORBIS_OK;
}

int32_t runtime_np_check_availability(void) {
    if (!runtime_net_is_online()) return NP_SIGNED_OUT;
    runtime_net_trace("sceNpCheckNpAvailability -> OK");
    return ORBIS_OK;
}

int32_t runtime_np_parental_control(int32_t req_id, void *online_or_user, int8_t *age, void *info) {
    (void)req_id; (void)online_or_user;
    if (age) *age = 21;
    /* SceNpParentalControlInfo is 3 bools (content/chat/ugc restriction).
     * Writing more than 3 bytes clobbers the caller's heap object. */
    if (info) memset(info, 0, 3);
    if (!runtime_net_is_online()) {
        runtime_net_trace("sceNpGetParentalControlInfo -> NP_SIGNED_OUT");
        return NP_SIGNED_OUT;
    }
    runtime_net_trace("sceNpGetParentalControlInfo -> age=21 (unrestricted)");
    return ORBIS_OK;
}

int32_t runtime_np_poll(int32_t req_id, int32_t *result) {
    (void)req_id;
    if (!result) return NP_INVALID_ARGUMENT;
    if (!runtime_net_is_online()) {
        *result = NP_SIGNED_OUT;
        runtime_net_trace("sceNpPollAsync(req=%d) -> NP_SIGNED_OUT", req_id);
    } else {
        *result = ORBIS_OK;
        runtime_net_trace("sceNpPollAsync(req=%d) -> ORBIS_OK (0)", req_id);
    }
    return ORBIS_OK;
}

int32_t runtime_np_auth_get_id_token(int32_t req_id, const void *param, void *token_buf) {
    (void)req_id; (void)param;
    if (!token_buf) return NP_INVALID_ARGUMENT;
    if (!runtime_net_is_online()) return NP_SIGNED_OUT;

    /* Write only the string + NUL: never assume the guest struct size. */
    OrbisNpIdToken *token = (OrbisNpIdToken *)token_buf;
    const char *name = get_online_name();
    snprintf(token->token, sizeof(token->token), "%s", name);
    runtime_net_trace("sceNpAuthGetIdTokenA -> '%s' (len=%zu)", token->token, strlen(token->token));
    return ORBIS_OK;
}

int32_t runtime_np_auth_get_auth_code(int32_t req_id, const void *param, void *code_buf, int32_t *issuer_id) {
    (void)req_id; (void)param;
    if (!code_buf) return NP_INVALID_ARGUMENT;
    if (!runtime_net_is_online()) return NP_SIGNED_OUT;

    OrbisNpAuthCode *code = (OrbisNpAuthCode *)code_buf;
    memcpy(code->code, "AUTH_CODE", 10);
    if (issuer_id) *issuer_id = 0x100;
    runtime_net_trace("sceNpAuthGetAuthorizationCode -> OK");
    return ORBIS_OK;
}

int32_t runtime_np_register_state_cb(void *cb, void *arg) {
    if (!cb) return NP_INVALID_ARGUMENT;
    g_np_state.legacy_cb = (NpStateCallbackLegacy)cb;
    g_np_state.legacy_arg = arg;
    g_np_state.legacy_pending = runtime_net_is_online() ? 1 : 0;
    runtime_net_trace("sceNpRegisterStateCallback -> legacy cb %p (pending=%d)", cb, g_np_state.legacy_pending);
    return ORBIS_OK;
}

int32_t runtime_np_register_state_cb_toolkit(void *cb, void *arg) {
    if (!cb) return NP_INVALID_ARGUMENT;
    g_np_state.toolkit_cb = (NpStateCallbackToolkit)cb;
    g_np_state.toolkit_arg = arg;
    g_np_state.toolkit_pending = runtime_net_is_online() ? 1 : 0;
    runtime_net_trace("sceNpRegisterStateCallbackForToolkit -> toolkit cb %p (pending=%d)", cb, g_np_state.toolkit_pending);
    return ORBIS_OK;
}

int32_t runtime_np_unregister_state_cb(void *cb, void *arg) {
    (void)arg;
    if (g_np_state.legacy_cb == (NpStateCallbackLegacy)cb) {
        g_np_state.legacy_cb = NULL;
        g_np_state.legacy_pending = 0;
    }
    return ORBIS_OK;
}

int32_t runtime_np_unregister_state_cb_toolkit(void *cb, void *arg) {
    (void)arg;
    if (g_np_state.toolkit_cb == (NpStateCallbackToolkit)cb) {
        g_np_state.toolkit_cb = NULL;
        g_np_state.toolkit_pending = 0;
    }
    return ORBIS_OK;
}

int32_t runtime_np_check_callback(void) {
    if (!runtime_net_is_online()) return ORBIS_OK;

    if (g_np_state.toolkit_pending && g_np_state.toolkit_cb) {
        g_np_state.toolkit_pending = 0;
        runtime_net_trace("sceNpCheckCallback -> notifying Toolkit callback: SIGNED_IN");
        g_np_state.toolkit_cb(1, ORBIS_NP_STATE_SIGNED_IN, g_np_state.toolkit_arg);
    }

    if (g_np_state.legacy_pending && g_np_state.legacy_cb) {
        g_np_state.legacy_pending = 0;
        OrbisNpId id;
        memset(&id, 0, sizeof(id));
        runtime_np_get_np_id(1, &id);
        runtime_net_trace("sceNpCheckCallback -> notifying legacy callback: SIGNED_IN");
        g_np_state.legacy_cb(1, ORBIS_NP_STATE_SIGNED_IN, &id, g_np_state.legacy_arg);
    }

    return ORBIS_OK;
}

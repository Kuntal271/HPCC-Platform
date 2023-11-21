/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2020 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "platform.h"
#include "jlog.hpp"
#include "jutil.hpp"
#include "jexcept.hpp"
#include "jmutex.hpp"
#include "jfile.hpp"
#include "jptree.hpp"
#include "jerror.hpp"
#include "jsecrets.hpp"

//including cpp-httplib single header file REST client
//  doesn't work with format-nonliteral as an error
//
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif

#ifdef _USE_OPENSSL
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif

#undef INVALID_SOCKET
#include "httplib.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef _USE_OPENSSL
#include <opensslcommon.hpp>
#include <openssl/x509v3.h>
#endif

//#define TRACE_SECRETS
#include <vector>

enum class CVaultKind { kv_v1, kv_v2 };

CVaultKind getSecretType(const char *s)
{
    if (isEmptyString(s))
        return CVaultKind::kv_v2;
    if (streq(s, "kv_v1"))
        return CVaultKind::kv_v1;
    return CVaultKind::kv_v2;
}
interface IVaultManager : extends IInterface
{
    virtual bool requestSecretFromVault(const char *category, const char *vaultId, CVaultKind &kind, StringBuffer &content, const char *secret, const char *version) = 0;
    virtual bool requestSecretByCategory(const char *category, CVaultKind &kind, StringBuffer &content, const char *secret, const char *version) = 0;
};

static Owned<IVaultManager> vaultManager;
static MemoryAttr udpKey;
static bool udpKeyInitialized = false;

static const IPropertyTree *getLocalSecret(const char *category, const char * name)
{
    return getSecret(category, name, "k8s", nullptr);
}

//based on kubernetes secret / key names. Even if some vault backends support additional characters we'll restrict to this subset for now

static const char *validSecretNameChrs = ".-";
inline static bool isValidSecretOrKeyNameChr(char c, bool firstOrLastChar, bool isKeyName)
{
    if (c == '\0')
        return false;
    if (isalnum(c))
        return true;
    if (firstOrLastChar)
        return false;
    if (strchr(validSecretNameChrs, c)!=nullptr)
        return true;
    return (isKeyName && c=='_'); //keyname also supports '_'
}

static bool isValidSecretOrKeyName(const char *name, bool isKeyName)
{
    if (!isValidSecretOrKeyNameChr(*name, true, isKeyName))
        return false;
    ++name;
    while ('\0' != *name)
    {
        bool lastChar = ('\0' == *(name+1));
        if (!isValidSecretOrKeyNameChr(*name, lastChar, isKeyName))
            return false;
        ++name;
    }
    return true;
}

static void validateCategoryName(const char *category)
{
    if (!isValidSecretOrKeyName(category, true))
      throw makeStringExceptionV(-1, "Invalid secret category %s", category);
}

static void validateSecretName(const char *secret)
{
    if (!isValidSecretOrKeyName(secret, false))
      throw makeStringExceptionV(-1, "Invalid secret name %s", secret);
}

static void validateKeyName(const char *key)
{
    if (!isValidSecretOrKeyName(key, true))
      throw makeStringExceptionV(-1, "Invalid secret key name %s", key);
}

static void splitUrlAddress(const char *address, size_t len, StringBuffer &host, StringBuffer &port)
{
    if (!address || len==0)
        return;
    const char *sep = (const char *)memchr(address, ':', len);
    if (!sep)
        host.append(len, address);
    else
    {
        host.append(sep - address, address);
        len = len - (sep - address) - 1;
        port.append(len, sep+1);
    }
}

static void splitUrlAuthority(const char *authority, size_t authorityLen, StringBuffer &user, StringBuffer &password, StringBuffer &host, StringBuffer &port)
{
    if (!authority || authorityLen==0)
        return;
    const char *at = (const char *) memchr(authority, '@', authorityLen);
    if (!at)
        splitUrlAddress(authority, authorityLen, host, port);
    else
    {
        size_t userinfoLen = (at - authority);
        splitUrlAddress(at+1, authorityLen - userinfoLen - 1, host, port);
        const char *sep = (const char *) memchr(authority, ':', at - authority);
        if (!sep)
            user.append(at-authority, authority);
        else
        {
            user.append(sep-authority, authority);
            size_t passwordLen = (at - sep - 1);
            password.append(passwordLen, sep+1);
        }
    }
}

static void splitUrlAuthorityHostPort(const char *authority, size_t authorityLen, StringBuffer &user, StringBuffer &password, StringBuffer &hostPort)
{
    StringBuffer port;
    splitUrlAuthority(authority, authorityLen, user, password, hostPort, port);
    if (port.length())
        hostPort.append(':').append(port);
}

static inline void extractUrlProtocol(const char *&url, StringBuffer *scheme)
{
    if (!url)
        throw makeStringException(-1, "Invalid empty URL");
    if (0 == strnicmp(url, "HTTPS://", 8))
    {
        url+=8;
        if (scheme)
            scheme->append("https://");
    }
    else if (0 == strnicmp(url, "HTTP://", 7))
    {
        url+=7;
        if (scheme)
            scheme->append("http://");
    }
    else
        throw MakeStringException(-1, "Invalid URL, protocol not recognized %s", url);
}

static void splitUrlSections(const char *url, const char * &authority, size_t &authorityLen, StringBuffer &fullpath, StringBuffer *scheme)
{
    extractUrlProtocol(url, scheme);
    const char* path = strchr(url, '/');
    authority = url;
    if (!path)
        authorityLen = strlen(authority);
    else
    {
        authorityLen = path-url;
        if (!streq(path, "/")) // treat empty trailing path as equal to no path
            fullpath.append(path);
    }
}

extern jlib_decl void splitFullUrl(const char *url, StringBuffer &user, StringBuffer &password, StringBuffer &host, StringBuffer &port, StringBuffer &path)
{
    const char *authority = nullptr;
    size_t authorityLen = 0;
    splitUrlSections(url, authority, authorityLen, path, nullptr);
    splitUrlAuthority(authority, authorityLen, user, password, host, port);
}

extern jlib_decl void splitUrlSchemeHostPort(const char *url, StringBuffer &user, StringBuffer &password, StringBuffer &schemeHostPort, StringBuffer &path)
{
    const char *authority = nullptr;
    size_t authorityLen = 0;
    splitUrlSections(url, authority, authorityLen, path, &schemeHostPort);
    splitUrlAuthorityHostPort(authority, authorityLen, user, password, schemeHostPort);
}

extern jlib_decl void splitUrlIsolateScheme(const char *url, StringBuffer &user, StringBuffer &password, StringBuffer &scheme, StringBuffer &host, StringBuffer &port, StringBuffer &path)
{
    const char *authority = nullptr;
    size_t authorityLen = 0;
    splitUrlSections(url, authority, authorityLen, path, &scheme);
    splitUrlAuthority(authority, authorityLen, user, password, host, port);
}


static StringBuffer &replaceExtraHostAndPortChars(StringBuffer &s)
{
    size_t l = s.length();
    for (size_t i = 0; i < l; i++)
    {
        if (s.charAt(i) == '.' || s.charAt(i) == ':')
            s.setCharAt(i, '-');
    }
    return s;
}


extern jlib_decl StringBuffer &generateDynamicUrlSecretName(StringBuffer &secretName, const char *scheme, const char *userPasswordPair, const char *host, unsigned port, const char *path)
{
    secretName.set("http-connect-");
    //Having the host and port visible will help with manageability wherever the secret is stored
    if (scheme)
    {
        if (!strnicmp("http", scheme, 4))
        {
            if ('s' == scheme[4])
            {
                if (443 == port)
                    port = 0; // suppress default port, such that with or without, the generated secret name will be the same
                secretName.append("ssl-");
            }
            else if (':' == scheme[4])
            {
                if (80 == port)
                    port = 0; // suppress default port, such that with or without, the generated secret name will be the same
            }
        }
    }
    secretName.append(host);
    //port is optionally already part of host
    replaceExtraHostAndPortChars(secretName);
    if (port)
        secretName.append('-').append(port);
    //Path and username are both sensitive and shouldn't be accessible in the name, include both in the hash to give us the uniqueness we need
    unsigned hashvalue = 0;
    if (!isEmptyString(path))
        hashvalue = hashcz((const unsigned char *)path, hashvalue);
    if (!isEmptyString(userPasswordPair))
    {
        const char *delim = strchr(userPasswordPair, ':');
        //Make unique for a given username, but not the current password.  The pw provided could change but what's in the secret (if there is one) wins
        if (delim)
            hashvalue = hashc((const unsigned char *)userPasswordPair, delim-userPasswordPair, hashvalue);
        else
            hashvalue = hashcz((const unsigned char *)userPasswordPair, hashvalue);
    }
    if (hashvalue)
        secretName.appendf("-%x", hashvalue);
    return secretName;
}

extern jlib_decl StringBuffer &generateDynamicUrlSecretName(StringBuffer &secretName, const char *url, const char *inputUsername)
{
    StringBuffer username;
    StringBuffer urlPassword;
    StringBuffer scheme;
    StringBuffer host;
    StringBuffer port;
    StringBuffer path;
    splitUrlIsolateScheme(url, username, urlPassword, scheme, host, port, path);
    if (!isEmptyString(inputUsername))
        username.set(inputUsername);
    unsigned portNum = port.length() ? atoi(port) : 0;
    return generateDynamicUrlSecretName(secretName, scheme, username, host, portNum, path);
}

//---------------------------------------------------------------------------------------------------------------------

static StringBuffer secretDirectory;
static CriticalSection secretCS;

//there are various schemes for renewing kubernetes secrets and they are likely to vary greatly in how often
//  a secret gets updated this timeout determines the maximum amount of time before we'll pick up a change
//  10 minutes for now we can change this as we gather more experience and user feedback
static unsigned secretTimeoutMs = 10 * 60 * 1000;

extern jlib_decl unsigned getSecretTimeout()
{
    return secretTimeoutMs;
}

extern jlib_decl void setSecretTimeout(unsigned timeoutMs)
{
    secretTimeoutMs = timeoutMs;
}

extern jlib_decl void setSecretMount(const char * path)
{
    if (!path)
    {
        getPackageFolder(secretDirectory);
        addPathSepChar(secretDirectory).append("secrets");
    }
    else
        secretDirectory.set(path);
}

static const char *ensureSecretDirectory()
{
    CriticalBlock block(secretCS);
    if (secretDirectory.isEmpty())
        setSecretMount(nullptr);
    return secretDirectory;
}

static StringBuffer &buildSecretPath(StringBuffer &path, const char *category, const char * name)
{
    return addPathSepChar(path.append(ensureSecretDirectory())).append(category).append(PATHSEPCHAR).append(name).append(PATHSEPCHAR);
}


enum class VaultAuthType {unknown, k8s, appRole, token, clientcert};

static void setTimevalMS(timeval &tv, time_t ms)
{
    if (!ms)
        tv = {0, 0};
    else
    {
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000)*1000;
    }
}

static bool isEmptyTimeval(const timeval &tv)
{
    return (tv.tv_sec==0 && tv.tv_usec==0);
}

//---------------------------------------------------------------------------------------------------------------------

//Represents an entry in the secret cache.  Once created it is always used for the secret.
using cache_timestamp = unsigned;
class SecretCacheEntry : public CInterface
{
    friend class SecretCache;

public:
    //A cache entry is initally created that has a create and access time of now, but the checkTimestamp
    //is set so that needsRefresh() will return true.
    SecretCacheEntry(cache_timestamp _now)
    : contentTimestamp(_now), accessedTimestamp(_now), checkedTimestamp(_now - 2 * secretTimeoutMs)
    {
    }

    unsigned getHash() const
    {
        return contentHash;
    }

    // We should never replace known contents for unknown contents
    // so once this returns true it should always return true
    bool hasContents() const
    {
        return contents != nullptr;
    }

    //Is the secret potentially out of date?
    bool isStale() const
    {
        cache_timestamp now = msTick();
        cache_timestamp elapsed = (now - contentTimestamp);
        return (elapsed  > secretTimeoutMs);
    }

    // Is it time to check if there is a new value for this secret?
    bool needsRefresh(cache_timestamp now) const
    {
        cache_timestamp elapsed = (now - checkedTimestamp);
        return (elapsed  > secretTimeoutMs);
    }

    bool needsRefresh() const
    {
        return needsRefresh(msTick());
    }

    void noteFailedUpdate(cache_timestamp now)
    {
        //Update the checked timestamp - so that we do not continually check for updates to secrets which
        //are stale because the vault or other source of values in inaccessible.
        //Keep using the last good value
        checkedTimestamp = now;
    }

    //The following functions can only be called from member functions of SecretCache
private:
    void updateContents(IPropertyTree * _contents, cache_timestamp now)
    {
        contents.set(_contents);
        updateHash();
        contentTimestamp = now;
        accessedTimestamp = now;
        checkedTimestamp = now;
    }

    void updateHash()
    {
        if (contents)
            contentHash = getPropertyTreeHash(*contents, 0x811C9DC5);
        else
            contentHash = 0;
    }
private:
    Linked<IPropertyTree> contents;// Can only be accessed when SecretCache::cs is held
    cache_timestamp contentTimestamp = 0;  // When was this secret read from disk/vault
    cache_timestamp accessedTimestamp = 0; // When was this secret last accessed?
    cache_timestamp checkedTimestamp = 0;  // When was this last checked for updates?
    unsigned contentHash = 0;
};

// A cache of (secret[:version] to a secret cache entry)
// Once a hash table entry has been created for a secret it is never removed and the associated
// value is never replaced.  This means it is safe to keep a pointer to the entry in another class.
class SecretCache
{
public:
    const IPropertyTree * getContents(SecretCacheEntry * match)
    {
        //Return contents within the critical section so no other thread can modify it
        CriticalBlock block(cs);
        return LINK(match->contents);
    }

    //Check to see if a secret exists, and if not add a null entry that has expired.
    SecretCacheEntry * resolveSecret(const std::string & secretKey, cache_timestamp now)
    {
        SecretCacheEntry * result;
        CriticalBlock block(cs);
        auto match = secrets.find(secretKey);
        if (match != secrets.cend())
        {
            result = match->second.get();
            result->accessedTimestamp = now;
        }
        else
        {
            //Insert an entry with a null value that is marked as out of date
            result = new SecretCacheEntry(now);
            secrets.emplace(secretKey, result);
        }
        return result;
    }

    void updateSecret(SecretCacheEntry * match, IPropertyTree * value, cache_timestamp now)
    {
        CriticalBlock block(cs);
        match->updateContents(value, now);
    }

private:
    CriticalSection cs;
    std::unordered_map<std::string, std::unique_ptr<SecretCacheEntry>> secrets;
};

//---------------------------------------------------------------------------------------------------------------------

class CVault
{
private:
    VaultAuthType authType = VaultAuthType::unknown;

    CVaultKind kind;
    CriticalSection vaultCS;

    std::string clientCertPath;
    std::string clientKeyPath;

    StringBuffer category;
    StringBuffer schemeHostPort;
    StringBuffer path;
    StringBuffer vaultNamespace;
    StringBuffer username;
    StringBuffer password;
    StringAttr name;

    StringAttr authRole; //authRole is used by kubernetes and client cert auth, it's not part of appRole auth
    StringAttr appRoleId;
    StringBuffer appRoleSecretName;

    StringBuffer clientToken;
    time_t clientTokenExpiration = 0;
    bool clientTokenRenewable = false;
    bool verify_server = true;
    unsigned retries = 3;
    unsigned retryWait = 1000;
    timeval connectTimeout = {0, 0};
    timeval readTimeout = {0, 0};
    timeval writeTimeout = {0, 0};

public:
    CVault(IPropertyTree *vault)
    {
        category.appendLower(vault->queryName());

        StringBuffer clientTlsPath;
        buildSecretPath(clientTlsPath, "certificates", "vaultclient");

        clientCertPath.append(clientTlsPath.str()).append(category.str()).append("/tls.crt");
        clientKeyPath.append(clientTlsPath.str()).append(category.str()).append("/tls.key");

        if (!checkFileExists(clientCertPath.c_str()))
            WARNLOG("vault: client cert not found, %s", clientCertPath.c_str());
        if (!checkFileExists(clientKeyPath.c_str()))
            WARNLOG("vault: client key not found, %s", clientKeyPath.c_str());

        StringBuffer url;
        replaceEnvVariables(url, vault->queryProp("@url"), false);
        PROGLOG("vault url %s", url.str());
        if (url.length())
            splitUrlSchemeHostPort(url.str(), username, password, schemeHostPort, path);

        if (username.length() || password.length())
            WARNLOG("vault: unexpected use of basic auth in url, user=%s", username.str());

        name.set(vault->queryProp("@name"));
        kind = getSecretType(vault->queryProp("@kind"));

        vaultNamespace.set(vault->queryProp("@namespace"));
        if (vaultNamespace.length())
        {
            addPathSepChar(vaultNamespace, '/');
            PROGLOG("vault: namespace %s", vaultNamespace.str());
        }
        verify_server = vault->getPropBool("@verify_server", true);
        retries = (unsigned) vault->getPropInt("@retries", retries);
        retryWait = (unsigned) vault->getPropInt("@retryWait", retryWait);

        setTimevalMS(connectTimeout, (time_t) vault->getPropInt("@connectTimeout"));
        setTimevalMS(readTimeout, (time_t) vault->getPropInt("@readTimeout"));
        setTimevalMS(writeTimeout, (time_t) vault->getPropInt("@writeTimeout"));

        PROGLOG("Vault: httplib verify_server=%s", boolToStr(verify_server));

        //set up vault client auth [appRole, clientToken (aka "token from the sky"), or kubernetes auth]
        appRoleId.set(vault->queryProp("@appRoleId"));
        if (appRoleId.length())
        {
            authType = VaultAuthType::appRole;
            if (vault->hasProp("@appRoleSecret"))
                appRoleSecretName.set(vault->queryProp("@appRoleSecret"));
            if (appRoleSecretName.isEmpty())
                appRoleSecretName.set("appRoleSecret");
        }
        else if (vault->hasProp("@client-secret"))
        {
            Owned<const IPropertyTree> clientSecret = getLocalSecret("system", vault->queryProp("@client-secret"));
            if (clientSecret)
            {
                StringBuffer tokenText;
                if (getSecretKeyValue(clientToken, clientSecret, "token"))
                {
                    authType = VaultAuthType::token;
                    PROGLOG("using a client token for vault auth");
                }
            }
        }
        else if (vault->getPropBool("@useTLSCertificateAuth", false))
        {
            authType = VaultAuthType::clientcert;
            if (vault->hasProp("@role"))
                authRole.set(vault->queryProp("@role"));
        }
        else if (isContainerized())
        {
            authType = VaultAuthType::k8s;
            if (vault->hasProp("@role"))
                authRole.set(vault->queryProp("@role"));
            else
                authRole.set("hpcc-vault-access");
            PROGLOG("using kubernetes vault auth");
        }
    }
    inline const char *queryAuthType()
    {
        switch (authType)
        {
            case VaultAuthType::appRole:
                return "approle";
            case VaultAuthType::k8s:
                return "kubernetes";
            case VaultAuthType::token:
                return "token";
            case VaultAuthType::clientcert:
                return "clientcert";
        }
        return "unknown";
    }
    void vaultAuthError(const char *msg)
    {
        Owned<IException> e = makeStringExceptionV(0, "Vault [%s] %s auth error %s", name.str(), queryAuthType(), msg);
        OERRLOG(e);
        throw e.getClear();
    }
    void vaultAuthErrorV(const char* format, ...) __attribute__((format(printf, 2, 3)))
    {
        va_list args;
        va_start(args, format);
        StringBuffer msg;
        msg.valist_appendf(format, args);
        va_end(args);
        vaultAuthError(msg);
    }
    void processClientTokenResponse(httplib::Result &res)
    {
        if (!res)
            vaultAuthErrorV("login communication error %d", res.error());
        if (res.error()!=0)
            OERRLOG("JSECRETS login calling HTTPLIB POST returned error %d", res.error());
        if (res->status != 200)
            vaultAuthErrorV("[%d](%d) - response: %s", res->status, res.error(), res->body.c_str());
        const char *json = res->body.c_str();
        if (isEmptyString(json))
            vaultAuthError("empty login response");

        Owned<IPropertyTree> respTree = createPTreeFromJSONString(json);
        if (!respTree)
            vaultAuthError("parsing JSON response");
        const char *token = respTree->queryProp("auth/client_token");
        if (isEmptyString(token))
            vaultAuthError("response missing client_token");

        clientToken.set(token);
        clientTokenRenewable = respTree->getPropBool("auth/renewable");
        unsigned lease_duration = respTree->getPropInt("auth/lease_duration");
        if (lease_duration==0)
            clientTokenExpiration = 0;
        else
            clientTokenExpiration = time(nullptr) + lease_duration;
        PROGLOG("VAULT TOKEN duration=%d", lease_duration);
    }
    bool isClientTokenExpired()
    {
        if (clientTokenExpiration==0)
            return false;

        double remaining = difftime(clientTokenExpiration, time(nullptr));
        if (remaining <= 0)
        {
            PROGLOG("vault auth client token expired");
            return true;
        }
        //TBD check renewal
        return false;
    }

    CVaultKind getVaultKind() const { return kind; }

    void initClient(httplib::Client &cli, httplib::Headers &headers, unsigned &numRetries)
    {
        numRetries = retries;
        cli.enable_server_certificate_verification(verify_server);
        if (!isEmptyTimeval(connectTimeout))
            cli.set_connection_timeout(connectTimeout.tv_sec, connectTimeout.tv_usec);
        if (!isEmptyTimeval(readTimeout))
            cli.set_read_timeout(readTimeout.tv_sec, readTimeout.tv_usec);
        if (!isEmptyTimeval(writeTimeout))
            cli.set_write_timeout(writeTimeout.tv_sec, writeTimeout.tv_usec);
        if (username.length() && password.length())
            cli.set_basic_auth(username, password);
        if (vaultNamespace.length())
            headers.emplace("X-Vault-Namespace", vaultNamespace.str());
    }

    //if we tried to use our token and it returned access denied it could be that we need to login again, or
    //  perhaps it could be specific permissions about the secret that was being accessed, I don't think we can tell the difference
    void kubernetesLogin(bool permissionDenied)
    {
        CriticalBlock block(vaultCS);
        if (!permissionDenied && (clientToken.length() && !isClientTokenExpired()))
            return;
        DBGLOG("kubernetesLogin%s", permissionDenied ? " because existing token permission denied" : "");
        StringBuffer login_token;
        login_token.loadFile("/var/run/secrets/kubernetes.io/serviceaccount/token");
        if (login_token.isEmpty())
            vaultAuthError("missing k8s auth token");

        std::string json;
        json.append("{\"jwt\": \"").append(login_token.str()).append("\", \"role\": \"").append(authRole.str()).append("\"}");
        httplib::Client cli(schemeHostPort.str());
        httplib::Headers headers;

        unsigned numRetries = 0;
        initClient(cli, headers, numRetries);
        httplib::Result res = cli.Post("/v1/auth/kubernetes/login", headers, json, "application/json");
        while (!res && numRetries--)
        {
            OERRLOG("Retrying vault %s kubernetes auth, communication error %d", name.str(), res.error());
            if (retryWait)
                Sleep(retryWait);
            res = cli.Post("/v1/auth/kubernetes/login", headers, json, "application/json");
        }

        processClientTokenResponse(res);
    }

    void clientCertLogin(bool permissionDenied)
    {
        CriticalBlock block(vaultCS);
        if (!permissionDenied && (clientToken.length() && !isClientTokenExpired()))
            return;
        DBGLOG("clientCertLogin%s", permissionDenied ? " because existing token permission denied" : "");

        std::string json;
        json.append("{\"name\": \"").append(authRole.str()).append("\"}"); //name can be empty but that is inefficient because vault would have to search for the cert being used

        httplib::Client cli(schemeHostPort.str(), clientCertPath, clientKeyPath);
        httplib::Headers headers;

        unsigned numRetries = 0;
        initClient(cli, headers, numRetries);
        httplib::Result res = cli.Post("/v1/auth/cert/login", headers, json, "application/json");
        while (!res && numRetries--)
        {
            OERRLOG("Retrying vault %s client cert auth, communication error %d", name.str(), res.error());
            if (retryWait)
                Sleep(retryWait);
            res = cli.Post("/v1/auth/cert/login", headers, json, "application/json");
        }

        processClientTokenResponse(res);
    }

    //if we tried to use our token and it returned access denied it could be that we need to login again, or
    //  perhaps it could be specific permissions about the secret that was being accessed, I don't think we can tell the difference
    void appRoleLogin(bool permissionDenied)
    {
        CriticalBlock block(vaultCS);
        if (!permissionDenied && (clientToken.length() && !isClientTokenExpired()))
            return;
        DBGLOG("appRoleLogin%s", permissionDenied ? " because existing token permission denied" : "");
        StringBuffer appRoleSecretId;
        Owned<const IPropertyTree> appRoleSecret = getLocalSecret("system", appRoleSecretName);
        if (!appRoleSecret)
            vaultAuthErrorV("appRole secret %s not found", appRoleSecretName.str());
        else if (!getSecretKeyValue(appRoleSecretId, appRoleSecret, "secret-id"))
            vaultAuthErrorV("appRole secret id not found at '%s/secret-id'", appRoleSecretName.str());
        if (appRoleSecretId.isEmpty())
            vaultAuthError("missing app-role-secret-id");

        std::string json;
        json.append("{\"role_id\": \"").append(appRoleId).append("\", \"secret_id\": \"").append(appRoleSecretId).append("\"}");

        httplib::Client cli(schemeHostPort.str());
        httplib::Headers headers;

        unsigned numRetries = 0;
        initClient(cli, headers, numRetries);
        httplib::Result res = cli.Post("/v1/auth/approle/login", headers, json, "application/json");
        while (!res && numRetries--)
        {
            OERRLOG("Retrying vault %s appRole auth, communication error %d", name.str(), res.error());
            if (retryWait)
                Sleep(retryWait);
            res = cli.Post("/v1/auth/approle/login", headers, json, "application/json");
        }

        processClientTokenResponse(res);
    }
    void checkAuthentication(bool permissionDenied)
    {
        if (authType == VaultAuthType::appRole)
            appRoleLogin(permissionDenied);
        else if (authType == VaultAuthType::k8s)
            kubernetesLogin(permissionDenied);
        else if (authType == VaultAuthType::clientcert)
            clientCertLogin(permissionDenied);
        else if (permissionDenied && authType == VaultAuthType::token)
            vaultAuthError("token permission denied"); //don't permenently invalidate token. Try again next time because it could be permissions for a particular secret rather than invalid token
        if (clientToken.isEmpty())
            vaultAuthError("no vault access token");
    }
    bool requestSecretAtLocation(CVaultKind &rkind, StringBuffer &content, const char *location, const char *secretCacheKey, const char *version, bool permissionDenied)
    {
        checkAuthentication(permissionDenied);
        if (isEmptyString(location))
        {
            OERRLOG("Vault %s cannot get secret at location without a location", name.str());
            return false;
        }

        httplib::Client cli(schemeHostPort.str());
        httplib::Headers headers = {
            { "X-Vault-Token", clientToken.str() }
        };

        unsigned numRetries = 0;
        initClient(cli, headers, numRetries);
        httplib::Result res = cli.Get(location, headers);
        while (!res && numRetries--)
        {
            OERRLOG("Retrying vault %s get secret, communication error %d location %s", name.str(), res.error(), location);
            if (retryWait)
                Sleep(retryWait);
            res = cli.Get(location, headers);
        }

        if (res)
        {
            if (res->status == 200)
            {
                rkind = kind;
                content.append(res->body.c_str());
                return true;
            }
            else if (res->status == 403)
            {
                 //try again forcing relogin, but only once.  Just in case the token was invalidated but hasn't passed expiration time (for example max usage count exceeded).
                if (permissionDenied==false)
                    return requestSecretAtLocation(rkind, content, location, secretCacheKey, version, true);
                OERRLOG("Vault %s permission denied accessing secret (check namespace=%s?) %s.%s location %s [%d](%d) - response: %s", name.str(), vaultNamespace.str(), secretCacheKey, version ? version : "", location, res->status, res.error(), res->body.c_str());
            }
            else if (res->status == 404)
            {
                OERRLOG("Vault %s secret not found %s.%s location %s", name.str(), secretCacheKey, version ? version : "", location);
            }
            else
            {
                OERRLOG("Vault %s error accessing secret %s.%s location %s [%d](%d) - response: %s", name.str(), secretCacheKey, version ? version : "", location, res->status, res.error(), res->body.c_str());
            }
        }
        else
            OERRLOG("Error: Vault %s http error (%d) accessing secret %s.%s location %s", name.str(), res.error(), secretCacheKey, version ? version : "", location);

        return false;
    }
    bool requestSecret(CVaultKind &rkind, StringBuffer &content, const char *secret, const char *version)
    {
        if (isEmptyString(secret))
            return false;

        StringBuffer location(path);
        location.replaceString("${secret}", secret);
        location.replaceString("${version}", version ? version : "1");

        return requestSecretAtLocation(rkind, content, location, secret, version, false);
    }
};

class CVaultSet
{
private:
    std::map<std::string, std::unique_ptr<CVault>> vaults;
public:
    CVaultSet()
    {
    }
    void addVault(IPropertyTree *vault)
    {
        const char *name = vault->queryProp("@name");
        if (!isEmptyString(name))
            vaults.emplace(name, std::unique_ptr<CVault>(new CVault(vault)));
    }
    bool requestSecret(CVaultKind &kind, StringBuffer &content, const char *secret, const char *version)
    {
        auto it = vaults.begin();
        for (; it != vaults.end(); it++)
        {
            if (it->second->requestSecret(kind, content, secret, version))
                return true;
        }
        return false;
    }
    bool requestSecretFromVault(const char *vaultId, CVaultKind &kind, StringBuffer &content, const char *secret, const char *version)
    {
        if (isEmptyString(vaultId))
            return false;
        auto it = vaults.find(vaultId);
        if (it == vaults.end())
            return false;
        return it->second->requestSecret(kind, content, secret, version);
    }
};

class CVaultManager : public CInterfaceOf<IVaultManager>
{
private:
    std::map<std::string, std::unique_ptr<CVaultSet>> categories;
public:
    CVaultManager()
    {
        Owned<const IPropertyTree> config;
        try
        {
            config.setown(getComponentConfigSP()->getPropTree("vaults"));
        }
        catch (IException * e)
        {
            EXCLOG(e);
            e->Release();
        }
        if (!config)
            return;
        Owned<IPropertyTreeIterator> iter = config->getElements("*");
        ForEach (*iter)
        {
            IPropertyTree &vault = iter->query();
            const char *category = vault.queryName();
            auto it = categories.find(category);
            if (it == categories.end())
            {
                auto placed = categories.emplace(category, std::unique_ptr<CVaultSet>(new CVaultSet()));
                if (placed.second)
                    it = placed.first;
            }
            if (it != categories.end())
                it->second->addVault(&vault);
        }
    }
    bool requestSecretFromVault(const char *category, const char *vaultId, CVaultKind &kind, StringBuffer &content, const char *secret, const char *version) override
    {
        if (isEmptyString(category))
            return false;
        auto it = categories.find(category);
        if (it == categories.end())
            return false;
        return it->second->requestSecretFromVault(vaultId, kind, content, secret, version);
    }

    bool requestSecretByCategory(const char *category, CVaultKind &kind, StringBuffer &content, const char *secret, const char *version) override
    {
        if (isEmptyString(category))
            return false;
        auto it = categories.find(category);
        if (it == categories.end())
            return false;
        return it->second->requestSecret(kind, content, secret, version);
    }
};

IVaultManager *ensureVaultManager()
{
    CriticalBlock block(secretCS);
    if (!vaultManager)
        vaultManager.setown(new CVaultManager());
    return vaultManager;
}

//---------------------------------------------------------------------------------------------------------------------

static SecretCache globalSecretCache;
static CriticalSection mtlsInfoCacheCS;
static std::unordered_map<std::string, Linked<ISyncedPropertyTree>> mtlsInfoCache;

MODULE_INIT(INIT_PRIORITY_SYSTEM)
{
    return true;
}

MODULE_EXIT()
{
    vaultManager.clear();
    udpKey.clear();
}


static IPropertyTree * resolveLocalSecret(const char *category, const char * name)
{
    StringBuffer path;
    buildSecretPath(path, category, name);

    Owned<IDirectoryIterator> entries = createDirectoryIterator(path);
    if (!entries || !entries->first())
        return nullptr;

    Owned<IPropertyTree> tree(createPTree(name));
    ForEach(*entries)
    {
        if (entries->isDir())
            continue;
        StringBuffer name;
        entries->getName(name);
        if (!validateXMLTag(name))
            continue;
        MemoryBuffer content;
        Owned<IFileIO> io = entries->query().open(IFOread);
        read(io, 0, (size32_t)-1, content);
        if (!content.length())
            continue;
        tree->setPropBin(name, content.length(), content.bufferBase());
    }

    return tree.getClear();
}

static IPropertyTree *createPTreeFromVaultSecret(const char *content, CVaultKind kind)
{
    if (isEmptyString(content))
        return nullptr;

    Owned<IPropertyTree> tree = createPTreeFromJSONString(content);
    if (!tree)
        return nullptr;
    switch (kind)
    {
        case CVaultKind::kv_v1:
            tree.setown(tree->getPropTree("data"));
            break;
        default:
        case CVaultKind::kv_v2:
            tree.setown(tree->getPropTree("data/data"));
            break;
    }
    return tree.getClear();
}

static IPropertyTree *resolveVaultSecret(const char *category, const char * name, const char *vaultId, const char *version)
{
    CVaultKind kind;
    StringBuffer json;
    IVaultManager *vaultmgr = ensureVaultManager();
    if (isEmptyString(vaultId))
    {
        if (!vaultmgr->requestSecretByCategory(category, kind, json, name, version))
            return nullptr;
    }
    else
    {
        if (!vaultmgr->requestSecretFromVault(category, vaultId, kind, json, name, version))
            return nullptr;
    }
    return createPTreeFromVaultSecret(json.str(), kind);
}


static SecretCacheEntry * getSecretEntry(const char *category, const char * name, const char * optVaultId, const char * optVersion)
{
    cache_timestamp now = msTick();

    std::string key;
    key.append(category).append("/").append(name);
    if (optVaultId)
        key.append("@").append(optVaultId);
    if (optVersion)
        key.append("#").append(optVersion);

    SecretCacheEntry * match = globalSecretCache.resolveSecret(key, now);
    if (!match->needsRefresh(now))
        return match;

    Owned<IPropertyTree> resolved;
    if (!isEmptyString(optVaultId))
    {
        if (strieq(optVaultId, "k8s"))
            resolved.setown(resolveLocalSecret(category, name));
        else
            resolved.setown(resolveVaultSecret(category, name, optVaultId, optVersion));
    }
    else
    {
        resolved.setown(resolveLocalSecret(category, name));
        if (!resolved)
            resolved.setown(resolveVaultSecret(category, name, nullptr, optVersion));
    }

    //If the secret could no longer be resolved (e.g. a vault has gone down) then keep the old one
    if (resolved)
        globalSecretCache.updateSecret(match, resolved, now);
    else
        match->noteFailedUpdate(now);

    return match;
}

static const IPropertyTree *getSecretTree(const char *category, const char * name, const char * optVaultId, const char * optVersion)
{
    SecretCacheEntry * secret = getSecretEntry(category, name, optVaultId, optVersion);
    if (secret)
        return globalSecretCache.getContents(secret);
    return nullptr;
}


//Public interface to the secrets

const IPropertyTree *getSecret(const char *category, const char * name, const char * optVaultId, const char * optVersion)
{
    validateCategoryName(category);
    validateSecretName(name);

    return getSecretTree(category,  name, optVaultId, optVersion);
}


bool getSecretKeyValue(MemoryBuffer & result, const IPropertyTree *secret, const char * key)
{
    validateKeyName(key);
    if (!secret)
        return false;
    return secret->getPropBin(key, result);
}

bool getSecretKeyValue(StringBuffer & result, const IPropertyTree *secret, const char * key)
{
    validateKeyName(key);
    if (!secret)
        return false;
    return secret->getProp(key, result);
}

extern jlib_decl bool getSecretValue(StringBuffer & result, const char *category, const char * name, const char * key, bool required)
{
    Owned<const IPropertyTree> secret = getSecret(category, name);
    if (required && !secret)
        throw MakeStringException(-1, "secret %s.%s not found", category, name);
    bool found = getSecretKeyValue(result, secret, key);
    if (required && !found)
        throw MakeStringException(-1, "secret %s.%s missing key %s", category, name, key);
    return true;
}

//---------------------------------------------------------------------------------------------------------------------

class CSecret final : public CInterfaceOf<ISyncedPropertyTree>
{
public:
    CSecret(const char *_category, const char * _name, const char * _vaultId, const char * _version, SecretCacheEntry * _secret)
    : category(_category), name(_name), vaultId(_vaultId), version(_version), secret(_secret)
    {
    }

    virtual const IPropertyTree * getTree() const override;

    virtual bool getProp(MemoryBuffer & result, const char * key) const override
    {
        CriticalBlock block(secretCs);
        checkUptoDate();
        Owned<const IPropertyTree> contents = globalSecretCache.getContents(secret);
        return getSecretKeyValue(result, contents, key);
    }
    virtual bool getProp(StringBuffer & result, const char * key) const override
    {
        CriticalBlock block(secretCs);
        checkUptoDate();
        Owned<const IPropertyTree> contents = globalSecretCache.getContents(secret);
        return getSecretKeyValue(result, contents, key);
    }
    virtual bool isStale() const override
    {
        return secret->isStale();
    }
    virtual unsigned getVersion() const override
    {
        CriticalBlock block(secretCs);
        checkUptoDate();
        return secret->getHash();
    }
    virtual bool isValid() const override
    {
        return secret->hasContents();
    }

protected:
    void checkUptoDate() const;

protected:
    StringAttr category;
    StringAttr name;
    StringAttr vaultId;
    StringAttr version;
    mutable CriticalSection secretCs;
    mutable SecretCacheEntry * secret;
};


const IPropertyTree * CSecret::getTree() const
{
    CriticalBlock block(secretCs);
    checkUptoDate();
    return globalSecretCache.getContents(secret);
}

void CSecret::checkUptoDate() const
{
    if (secret->needsRefresh())
    {
#ifdef TRACE_SECRETS
        DBGLOG("Secret %s/%s is stale updating from %u...", category.str(), name.str(), secretHash);
#endif
        //MORE: This could block or fail - in roxie especially it would be better to return the old value
        try
        {
            SecretCacheEntry * newSecret = getSecretEntry(category, name, vaultId, version);
            //Check the secret is always returned consistently.  It would be possible to call a slightly
            //more optimal function to refresh a secret, but this is simplest.
            assertex(secret == newSecret);
        }
        catch (IException * e)
        {
            VStringBuffer msg("Failed to update secret %s.%s", category.str(), name.str());
            EXCLOG(e, msg.str());
            e->Release();
        }
    }
}

ISyncedPropertyTree * resolveSecret(const char *category, const char * name, const char * optVaultId, const char * optVersion)
{
    validateCategoryName(category);
    validateSecretName(name);

    SecretCacheEntry * resolved = getSecretEntry(category, name, optVaultId, optVersion);
    return new CSecret(category, name, optVaultId, optVersion, resolved);
}

//---------------------------------------------------------------------------------------------------------------------

void initSecretUdpKey()
{
    if (udpKeyInitialized)
        return;

//can find alternatives for old openssl in the future if necessary
#if defined(_USE_OPENSSL) && (OPENSSL_VERSION_NUMBER >= 0x10100000L)
    StringBuffer path;
    BIO *in = BIO_new_file(buildSecretPath(path, "certificates", "udp").append("tls.key"), "r");
    if (in == nullptr)
        return;
    EC_KEY *eckey = PEM_read_bio_ECPrivateKey(in, nullptr, nullptr, nullptr);
    if (eckey)
    {
        unsigned char *priv = NULL;
        size_t privlen = EC_KEY_priv2buf(eckey, &priv);
        if (privlen != 0)
        {
            udpKey.set(privlen, priv);
            OPENSSL_clear_free(priv, privlen);
        }
        EC_KEY_free(eckey);
    }
    BIO_free(in);
#endif
    udpKeyInitialized = true;
}

const MemoryAttr &getSecretUdpKey(bool required)
{
    if (!udpKeyInitialized)
        throw makeStringException(-1, "UDP Key not initialized.");
    if (required && !udpKey.length())
        throw makeStringException(-1, "UDP Key not found, cert-manager integration/configuration required.");
    return udpKey;
}

jlib_decl bool containsEmbeddedKey(const char *certificate)
{
    // look for any of:
    // -----BEGIN PRIVATE KEY-----
    // -----BEGIN RSA PRIVATE KEY-----
    // -----BEGIN CERTIFICATE-----
    // -----BEGIN PUBLIC KEY-----
    // or maybe just:
    // -----BEGIN

    if ( (strstr(certificate, "-----BEGIN PRIVATE KEY-----")) ||
         (strstr(certificate, "-----BEGIN RSA PRIVATE KEY-----")) ||
         (strstr(certificate, "-----BEGIN PUBLIC KEY-----")) ||
         (strstr(certificate, "-----BEGIN CERTIFICATE-----")) )
        return true;

    return false;
}


//---------------------------------------------------------------------------------------------------------------------

class CSyncedCertificateBase : public CInterfaceOf<ISyncedPropertyTree>
{
public:
    CSyncedCertificateBase(const char *_issuer)
    : issuer(_issuer)
    {
    }

    virtual const IPropertyTree * getTree() const override final;

    virtual bool getProp(MemoryBuffer & result, const char * key) const override final
    {
        CriticalBlock block(secretCs);
        checkUptoDate();
        return getSecretKeyValue(result, config, key);
    }
    virtual bool getProp(StringBuffer & result, const char * key) const override final
    {
        CriticalBlock block(secretCs);
        checkUptoDate();
        return getSecretKeyValue(result, config, key);
    }
    virtual bool isStale() const override final
    {
        return secret->isStale();
    }
    virtual bool isValid() const override
    {
        return secret->isValid();

    }
    virtual unsigned getVersion() const override final
    {
        CriticalBlock block(secretCs);
        checkUptoDate();
        //If information that is combined with the secret (e.g. trusted peers) can also change dynamically this would
        //need to be a separate hash calculated from the config tree
        return secretHash;
    }

protected:
    virtual void updateConfigFromSecret(const IPropertyTree * secretInfo) const = 0;

protected:
    void checkUptoDate() const;
    void createConfig() const;
    void createDefaultConfigFromSecret(const IPropertyTree * secretInfo, bool addCertificates, bool addCertificateAuthority) const;
    void updateCertificateFromSecret(const IPropertyTree * secretInfo) const;
    void updateCertificateAuthorityFromSecret(const IPropertyTree * secretInfo) const;

protected:
    StringAttr issuer;
    Owned<ISyncedPropertyTree> secret;
    mutable CriticalSection secretCs;
    mutable Linked<IPropertyTree> config;
    mutable std::atomic<unsigned> secretHash{0};
};


const IPropertyTree * CSyncedCertificateBase::getTree() const
{
    CriticalBlock block(secretCs);
    checkUptoDate();
    return LINK(config);
}

void CSyncedCertificateBase::checkUptoDate() const
{
    if (secretHash != secret->getVersion())
        createConfig();
}

void CSyncedCertificateBase::createConfig() const
{
    //Update before getting the tree to avoid potential race condition updating the tree at the same time.
    //Could alternatively return the version number from the getTree() call.
    secretHash = secret->getVersion();

    Owned<const IPropertyTree> secretInfo = secret->getTree();
    if (secretInfo)
    {
        config.setown(createPTree(issuer));
        ensurePTree(config, "verify");
        updateConfigFromSecret(secretInfo);
    }
    else
        config.clear();
}


void CSyncedCertificateBase::updateCertificateFromSecret(const IPropertyTree * secretInfo) const
{
    StringBuffer value;
    config->setProp("@issuer", issuer); // server only?
    if (secretInfo->getProp("tls.crt", value.clear()))
        config->setProp("certificate", value.str());
    if (secretInfo->getProp("tls.key", value.clear()))
        config->setProp("privatekey", value.str());
}

void CSyncedCertificateBase::updateCertificateAuthorityFromSecret(const IPropertyTree * secretInfo) const
{
    StringBuffer value;
    if (secretInfo->getProp("ca.crt", value.clear()))
    {
        IPropertyTree *verify = config->queryPropTree("verify");
        IPropertyTree *ca = ensurePTree(verify, "ca_certificates");
        ca->setProp("pem", value.str());
    }
}


//---------------------------------------------------------------------------------------------------------------------

class CIssuerConfig final : public CSyncedCertificateBase
{
public:
    CIssuerConfig(const char *_issuer, const char * _trustedPeers, bool _isClientConnection, bool _acceptSelfSigned, bool _addCACert, bool _disableMTLS)
    : CSyncedCertificateBase(_issuer), trustedPeers(_trustedPeers), isClientConnection(_isClientConnection), acceptSelfSigned(_acceptSelfSigned), addCACert(_addCACert), disableMTLS(_disableMTLS)
    {
        secret.setown(resolveSecret("certificates", issuer, nullptr, nullptr));
        createConfig();
    }

    virtual void updateConfigFromSecret(const IPropertyTree * secretInfo) const override;

protected:
    StringAttr trustedPeers;
    bool isClientConnection; // required in constructor
    bool acceptSelfSigned; // required in constructor
    bool addCACert; // required in constructor
    bool disableMTLS;
};


void CIssuerConfig::updateConfigFromSecret(const IPropertyTree * secretInfo) const
{
    if (!isClientConnection || !strieq(issuer, "public"))
        updateCertificateFromSecret(secretInfo);


    // addCACert is usually true. A client hitting a public issuer is the case where we don't want the ca cert
    // defined. Otherwise, for MTLS we want control over our CACert using addCACert. When hitting public services
    // using public certificate authorities we want the well known (browser compatible) CA list installed on the
    // system instead.
    if (!isClientConnection || addCACert)
        updateCertificateAuthorityFromSecret(secretInfo);

    IPropertyTree *verify = config->queryPropTree("verify");
    assertex(verify); // Should always be defined by this point.

    //For now only the "public" issuer implies client certificates are not required
    verify->setPropBool("@enable", !disableMTLS && (isClientConnection || !strieq(issuer, "public")));
    verify->setPropBool("@address_match", false);
    verify->setPropBool("@accept_selfsigned", isClientConnection && acceptSelfSigned);
    if (trustedPeers) // Allow blank string to mean none, null means anyone
        verify->setProp("trusted_peers", trustedPeers);
    else
        verify->setProp("trusted_peers", "anyone");
}


ISyncedPropertyTree * createIssuerTlsConfig(const char * issuer, const char * optTrustedPeers, bool isClientConnection, bool acceptSelfSigned, bool addCACert, bool disableMTLS)
{
    return new CIssuerConfig(issuer, optTrustedPeers, isClientConnection, acceptSelfSigned, addCACert, disableMTLS);

}
//---------------------------------------------------------------------------------------------------------------------

class CCertificateConfig final : public CSyncedCertificateBase
{
public:
    CCertificateConfig(const char * _category, const char * _secretName, bool _addCACert)
    : CSyncedCertificateBase(nullptr), addCACert(_addCACert)
    {
        secret.setown(resolveSecret(_category, _secretName, nullptr, nullptr));
        if (!secret->isValid())
            throw makeStringExceptionV(-1, "secret %s.%s not found", _category, _secretName);
        createConfig();
    }

    virtual void updateConfigFromSecret(const IPropertyTree * secretInfo) const override;

protected:
    bool addCACert; // required in constructor
};

void CCertificateConfig::updateConfigFromSecret(const IPropertyTree * secretInfo) const
{
    updateCertificateFromSecret(secretInfo);

    if (addCACert)
        updateCertificateAuthorityFromSecret(secretInfo);
}


ISyncedPropertyTree * createStorageTlsConfig(const char * secretName, bool addCACert)
{
    return new CCertificateConfig("storage", secretName, addCACert);

}


const ISyncedPropertyTree * getIssuerTlsSyncedConfig(const char * issuer, const char * optTrustedPeers, bool disableMTLS)
{
    if (isEmptyString(issuer))
        return nullptr;

    const char * key;
    StringBuffer temp;
    if (!isEmptyString(optTrustedPeers) || disableMTLS)
    {
        temp.append(issuer).append("/").append(optTrustedPeers).append('/').append(disableMTLS);
        key = temp.str();
    }
    else
        key = issuer;

    CriticalBlock block(mtlsInfoCacheCS);
    auto match = mtlsInfoCache.find(key);
    if (match != mtlsInfoCache.cend())
        return LINK(match->second);

    Owned<ISyncedPropertyTree> config = createIssuerTlsConfig(issuer, optTrustedPeers, false, false, true, disableMTLS);
    mtlsInfoCache.emplace(key, config);
    return config.getClear();
}

bool hasIssuerTlsConfig(const char *issuer)
{
    Owned<const ISyncedPropertyTree> match = getIssuerTlsSyncedConfig(issuer, nullptr, false);
    return match && match->isValid();
}

enum UseMTLS { UNINIT, DISABLED, ENABLED };
static UseMTLS useMtls = UNINIT;

static CriticalSection queryMtlsCS;

jlib_decl bool queryMtls()
{
    CriticalBlock block(queryMtlsCS);
    if (useMtls == UNINIT)
    {
        useMtls = DISABLED;
#if defined(_USE_OPENSSL)
# ifdef _CONTAINERIZED
        // check component setting first, but default to global
        if (getComponentConfigSP()->getPropBool("@mtls", getGlobalConfigSP()->getPropBool("security/@mtls")))
            useMtls = ENABLED;
# else
        if (queryMtlsBareMetalConfig())
        {
            useMtls = ENABLED;
            const char *cert = nullptr;
            const char *pubKey = nullptr;
            const char *privKey = nullptr;
            const char *passPhrase = nullptr;
            if (queryHPCCPKIKeyFiles(&cert, &pubKey, &privKey, &passPhrase))
            {
                if ( (!isEmptyString(cert)) && (!isEmptyString(privKey)) )
                {
                    if (checkFileExists(cert) && checkFileExists(privKey))
                    {
                        CriticalBlock block(mtlsInfoCacheCS);
                        assertex(mtlsInfoCache.find("local") == mtlsInfoCache.cend());

                        Owned<IPropertyTree> info = createPTree("local");
                        info->setProp("certificate", cert);
                        info->setProp("privatekey", privKey);
                        if ( (!isEmptyString(pubKey)) && (checkFileExists(pubKey)) )
                            info->setProp("publickey", pubKey);
                        if (!isEmptyString(passPhrase))
                            info->setProp("passphrase", passPhrase); // encrypted

                        Owned<ISyncedPropertyTree> entry = createSyncedPropertyTree(info);
                        mtlsInfoCache.emplace("local", entry);
                    }
                }
            }
        }
# endif
#endif
    }
    if (useMtls == ENABLED)
        return true;
    else
        return false;
}

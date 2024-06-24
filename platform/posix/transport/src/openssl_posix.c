/* Standard includes. */
#include <assert.h>
#include <string.h>

/* POSIX socket includes. */
#include <unistd.h>
#include <poll.h>

/* Transport interface include. */
#include "transport_interface.h"

#include "openssl_posix.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

/* Openssl Provider includes*/
#include <openssl/provider.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/core_names.h>

/*-----------------------------------------------------------*/

/**
 * @brief Label of root CA when calling @ref logPath.
 */
#define ROOT_CA_LABEL        "Root CA certificate"

/**
 * @brief Label of client certificate when calling @ref logPath.
 */
#define CLIENT_CERT_LABEL    "client's certificate"

/**
 * @brief Label of client key when calling @ref logPath.
 */
#define CLIENT_KEY_LABEL     "client's key"

#define PRIVATE_KEYSLOT "0xe0f1"

#define TRUSTM_PROVIDER_PATH "/home/pi/optiga-trust-m-explorer-aws/Python_TrustM_GUI/linux-optiga-trust-m/bin/trustm_provider.so"

/*-----------------------------------------------------------*/

/* Each compilation unit must define the NetworkContext struct. */
struct NetworkContext
{
    OpensslParams_t * pParams;
};

/*-----------------------------------------------------------*/
// Function declarations
static EVP_PKEY *load_private_key_from_hsm(const char *keyIdentifier);
static EVP_PKEY *load_public_key_from_cert(const char *cert_path);
static int compare_public_keys(EVP_PKEY *key1, EVP_PKEY *key2);
static void print_openssl_errors(void);

/**
 * @brief Log the absolute path given a relative or absolute path.
 *
 * @param[in] path Relative or absolute path.
 * @param[in] fileType NULL-terminated string describing the file type to log.
 */
#if ( LIBRARY_LOG_LEVEL == LOG_DEBUG )
    static void logPath( const char * path,
                         const char * fileType );
#endif /* #if ( LIBRARY_LOG_LEVEL == LOG_DEBUG ) */

/**
 * @brief Add X509 certificate to the trusted list of root certificates.
 *
 * OpenSSL does not provide a single function for reading and loading
 * certificates from files into stores, so the file API must be called. Start
 * with the root certificate.
 *
 * @param[out] pSslContext SSL context to which the trusted server root CA is to
 * be added.
 * @param[in] pRootCaPath Filepath string to the trusted server root CA.
 *
 * @return 1 on success; -1, 0 on failure.
 */
static int32_t setRootCa( const SSL_CTX * pSslContext,
                          const char * pRootCaPath );

/**
 * @brief Set X509 certificate as client certificate for the server to
 * authenticate.
 *
 * @param[out] pSslContext SSL context to which the client certificate is to be
 * set.
 * @param[in] pClientCertPath Filepath string to the client certificate.
 *
 * @return 1 on success; 0 failure.
 */
static int32_t setClientCertificate( SSL_CTX * pSslContext,
                                     const char * pClientCertPath );

/**
 * @brief Set private key for the client's certificate.
 *
 * @param[out] pSslContext SSL context to which the private key is to be added.
 * @param[in] pPrivateKey Private key to be used by the client.
 *
 * @return 1 on success; 0 on failure.
 */
static int32_t setPrivateKey( SSL_CTX * pSslContext,
                              EVP_PKEY * pPrivateKey );

/**
 * @brief Passes TLS credentials to the OpenSSL library.
 *
 * Provides the root CA certificate, client certificate, and private key to the
 * OpenSSL library. If the client certificate or private key is not NULL, mutual
 * authentication is used when performing the TLS handshake.
 *
 * @param[out] pSslContext SSL context to which the credentials are to be
 * imported.
 * @param[in] pOpensslCredentials TLS credentials to be imported.
 *
 * @return 1 on success; -1, 0 on failure.
 */
static int32_t setCredentials( SSL_CTX * pSslContext,
                               const OpensslCredentials_t * pOpensslCredentials );

/**
 * @brief Set optional configurations for the TLS connection.
 *
 * This function is used to set SNI, MFLN, and ALPN protocols.
 *
 * @param[in] pSsl SSL context to which the optional configurations are to be
 * set.
 * @param[in] pOpensslCredentials TLS credentials containing configurations.
 */
static void setOptionalConfigurations( SSL * pSsl,
                                       const OpensslCredentials_t * pOpensslCredentials );

/**
 * @brief Converts the sockets wrapper status to openssl status.
 *
 * @param[in] socketStatus Sockets wrapper status.
 *
 * @return #OPENSSL_SUCCESS, #OPENSSL_INVALID_PARAMETER, #OPENSSL_DNS_FAILURE,
 * and #OPENSSL_CONNECT_FAILURE.
 */
static OpensslStatus_t convertToOpensslStatus( SocketStatus_t socketStatus );

/**
 * @brief Establish TLS session by performing handshake with the server.
 *
 * @param[in] pServerInfo Server connection info.
 * @param[in] pOpensslParams Parameters to perform the TLS handshake.
 * @param[in] pOpensslCredentials TLS credentials containing configurations.
 *
 * @return #OPENSSL_SUCCESS, #OPENSSL_API_ERROR, and #OPENSSL_HANDSHAKE_FAILED.
 */
static OpensslStatus_t tlsHandshake( const ServerInfo_t * pServerInfo,
                                     OpensslParams_t * pOpensslParams,
                                     const OpensslCredentials_t * pOpensslCredentials );

/**
 * @brief Check if the network context is valid.
 *
 * @param[in] pNetworkContext The network context created using Openssl_Connect API.
 *
 * @return 1 on success; 0 on failure.
 */
static int32_t isValidNetworkContext( const NetworkContext_t * pNetworkContext );
/*-----------------------------------------------------------*/
OSSL_PROVIDER *trustm_provider = NULL;

static int initialize_trustm_provider(void) {
    trustm_provider = OSSL_PROVIDER_load(NULL, TRUSTM_PROVIDER_PATH);
    if (trustm_provider == NULL) {
        LogError(("Failed to load TrustM provider\n"));
        print_openssl_errors();
        return 0;
    }
    LogDebug(("TrustM provider loaded successfully\n"));
    return 1;
}

/*-----------------------------------------------------------*/

#if ( LIBRARY_LOG_LEVEL == LOG_DEBUG )
    static void logPath( const char * path,
                         const char * fileType )
    {
        char * cwd = NULL;

        assert( path != NULL );
        assert( fileType != NULL );

        /* Unused parameter when logs are disabled. */
        ( void ) fileType;

        /* Log the absolute directory based on first character of path. */
        if( ( path[ 0 ] == '/' ) || ( path[ 0 ] == '\\' ) )
        {
            LogDebug( ( "Attempting to open %s: Path=%s.", fileType, path ) );
        }
        else
        {
            cwd = getcwd( NULL, 0 );
            LogDebug( ( "Attempting to open %s: Path=%s/%s.", fileType, cwd, path ) );
        }

        /* Free cwd because getcwd calls malloc. */
        free( cwd );
    }
#endif /* #if ( LIBRARY_LOG_LEVEL == LOG_DEBUG ) */
/*-----------------------------------------------------------*/

static OpensslStatus_t convertToOpensslStatus( SocketStatus_t socketStatus )
{
    OpensslStatus_t opensslStatus = OPENSSL_INVALID_PARAMETER;

    switch( socketStatus )
    {
        case SOCKETS_SUCCESS:
            opensslStatus = OPENSSL_SUCCESS;
            break;

        case SOCKETS_INVALID_PARAMETER:
            opensslStatus = OPENSSL_INVALID_PARAMETER;
            break;

        case SOCKETS_DNS_FAILURE:
            opensslStatus = OPENSSL_DNS_FAILURE;
            break;

        case SOCKETS_CONNECT_FAILURE:
            opensslStatus = OPENSSL_CONNECT_FAILURE;
            break;

        default:
            LogError(
                ( "Unexpected status received from socket wrapper: Socket status = %u",
                  socketStatus ) );
            break;
    }

    return opensslStatus;
}
/*-----------------------------------------------------------*/

static OpensslStatus_t tlsHandshake( const ServerInfo_t * pServerInfo,
                                     OpensslParams_t * pOpensslParams,
                                     const OpensslCredentials_t * pOpensslCredentials )
{
    OpensslStatus_t returnStatus = OPENSSL_SUCCESS;
    int32_t sslStatus = -1, verifyPeerCertStatus = X509_V_OK;

    /* Validate the hostname against the server's certificate. */
    sslStatus = SSL_set1_host( pOpensslParams->pSsl, pServerInfo->pHostName );

    if (sslStatus != 1) {
        LogError(("SSL_set1_host failed to set the hostname to validate.\n"));
        return OPENSSL_API_ERROR;
    }

    /* Verify the server certificate */
    verifyPeerCertStatus = ( int32_t ) SSL_get_verify_result( pOpensslParams->pSsl );

    if (verifyPeerCertStatus != X509_V_OK) {
        LogError(("SSL_get_verify_result failed to verify X509 certificate from peer.\n"));
        return OPENSSL_HANDSHAKE_FAILED;
    }

    /* Enable SSL peer verification. */
    if( returnStatus == OPENSSL_SUCCESS )
    {
        SSL_set_verify( pOpensslParams->pSsl, SSL_VERIFY_PEER, NULL );

        /* Setup the socket to use for communication. */
        sslStatus =
            SSL_set_fd( pOpensslParams->pSsl, pOpensslParams->socketDescriptor );

        if( sslStatus != 1 )
        {
            LogError( ( "SSL_set_fd failed to set the socket fd to SSL context." ) );
            returnStatus = OPENSSL_API_ERROR;
        }
    }

    /* Perform the TLS handshake. */
    if( returnStatus == OPENSSL_SUCCESS )
    {
        setOptionalConfigurations( pOpensslParams->pSsl, pOpensslCredentials );

        sslStatus = SSL_connect( pOpensslParams->pSsl );

        if( sslStatus != 1 )
        {
            LogError( ( "SSL_connect failed to perform TLS handshake." ) );
            returnStatus = OPENSSL_HANDSHAKE_FAILED;
        }
    }

    /* Verify X509 certificate from peer. */
    if( returnStatus == OPENSSL_SUCCESS )
    {
        verifyPeerCertStatus = ( int32_t ) SSL_get_verify_result( pOpensslParams->pSsl );

        if( verifyPeerCertStatus != X509_V_OK )
        {
            LogError( ( "SSL_get_verify_result failed to verify X509 "
                        "certificate from peer." ) );
            returnStatus = OPENSSL_HANDSHAKE_FAILED;
        }
    }

    return returnStatus;
}

static int32_t setRootCa( const SSL_CTX * pSslContext,
                          const char * pRootCaPath )
{
    int32_t sslStatus = 1;
    FILE * pRootCaFile = NULL;
    X509 * pRootCa = NULL;

    assert( pSslContext != NULL );
    assert( pRootCaPath != NULL );

    #if ( LIBRARY_LOG_LEVEL == LOG_DEBUG )
        logPath( pRootCaPath, ROOT_CA_LABEL );
    #endif

    /* MISRA Rule 21.6 flags the following line for using the standard
     * library input/output function `fopen()`. This rule is suppressed because
     * openssl function #PEM_read_X509 takes an argument of type `FILE *` for
     * reading the root ca PEM file and `fopen()` needs to be used to get the
     * file pointer.  */
    /* coverity[misra_c_2012_rule_21_6_violation] */
    pRootCaFile = fopen( pRootCaPath, "r" );

    if( pRootCaFile == NULL )
    {
        LogError( ( "fopen failed to find the root CA certificate file: "
                    "ROOT_CA_PATH=%s.",
                    pRootCaPath ) );
        sslStatus = -1;
    }

    if( sslStatus == 1 )
    {
        /* Read the root CA into an X509 object. */
        pRootCa = PEM_read_X509( pRootCaFile, NULL, NULL, NULL );

        if( pRootCa == NULL )
        {
            LogError( ( "PEM_read_X509 failed to parse root CA." ) );
            sslStatus = -1;
        }
    }

    if( sslStatus == 1 )
    {
        /* Add the certificate to the context. */
        sslStatus =
            X509_STORE_add_cert( SSL_CTX_get_cert_store( pSslContext ), pRootCa );

        if( sslStatus != 1 )
        {
            LogError(
                ( "X509_STORE_add_cert failed to add root CA to certificate store." ) );
            sslStatus = -1;
        }
    }

    /* Free the X509 object used to set the root CA. */
    if( pRootCa != NULL )
    {
        X509_free( pRootCa );
        pRootCa = NULL;
    }

    /* Close the file if it was successfully opened. */
    if( pRootCaFile != NULL )
    {
        /* MISRA Rule 21.6 flags the following line for using the standard
         * library input/output function `fclose()`. This rule is suppressed
         * because openssl function #PEM_read_X509 takes an argument of type
         * `FILE *` for reading the root ca PEM file and `fopen()` is used to
         * get the file pointer. The file opened with `fopen()` needs to be
         * closed by calling `fclose()`.*/
        /* coverity[misra_c_2012_rule_21_6_violation] */
        if( fclose( pRootCaFile ) != 0 )
        {
            LogWarn( ( "fclose failed to close file %s", pRootCaPath ) );
        }
    }

    /* Log the success message if we successfully imported the root CA. */
    if( sslStatus == 1 )
    {
        LogInfo( ( "Successfully imported root CA." ) );
    }

    return sslStatus;
}
/*-----------------------------------------------------------*/

static int32_t setClientCertificate( SSL_CTX * pSslContext,
                                     const char * pClientCertPath )
{
    int32_t sslStatus = -1;

    assert( pSslContext != NULL );
    assert( pClientCertPath != NULL );

    #if ( LIBRARY_LOG_LEVEL == LOG_DEBUG )
        logPath( pClientCertPath, CLIENT_CERT_LABEL );
    #endif

    LogDebug(("Client certificate path: %s", pClientCertPath));

    /* Import the client certificate. */
    sslStatus = SSL_CTX_use_certificate_chain_file( pSslContext, pClientCertPath );

    if( sslStatus != 1 )
    {
        LogError( ( "SSL_CTX_use_certificate_chain_file failed to import "
                    "client certificate at %s.",
                    pClientCertPath ) );
    }
    else
    {
        LogDebug( ( "Successfully imported client certificate." ) );
    }

    return sslStatus;
}
/*-----------------------------------------------------------*/

static int32_t setPrivateKey( SSL_CTX * pSslContext, EVP_PKEY * pPrivateKey )
{
    int32_t sslStatus = -1;

    assert( pSslContext != NULL );
    assert( pPrivateKey != NULL );

    /* Import the client certificate private key. */
    sslStatus = SSL_CTX_use_PrivateKey( pSslContext, pPrivateKey );

    if( sslStatus != 1 )
    {
        LogError( ( "SSL_CTX_use_PrivateKey failed to import client "
                    "certificate private key." ) );
    }
    else
    {
        LogDebug( ( "Successfully imported client certificate private key." ) );
    }

    return sslStatus;
}
/*-----------------------------------------------------------*/
static int32_t setCredentials(SSL_CTX *pSslContext, const OpensslCredentials_t *pOpensslCredentials)
{
    int32_t sslStatus = -1;
    char error_buf[256]; // Buffer for error messages

    assert(pSslContext != NULL);
    assert(pOpensslCredentials != NULL);

    /* Set the client certificate first */
    if (pOpensslCredentials->pClientCertPath != NULL)
    {
        LogDebug(("Client certificate path: %s", pOpensslCredentials->pClientCertPath));
        sslStatus = SSL_CTX_use_certificate_file(pSslContext, pOpensslCredentials->pClientCertPath, SSL_FILETYPE_PEM);

        if (sslStatus != 1)
        {
            print_openssl_errors();
            LogError(("SSL_CTX_use_certificate_file failed to load client certificate."));
            return sslStatus;
        }
    }
    else
    {
        LogError(("Client certificate path is NULL."));
        return -1;
    }

    /* Now set the private key */
    if (pOpensslCredentials->pPrivateKey != NULL)
    {
        sslStatus = SSL_CTX_use_PrivateKey(pSslContext, pOpensslCredentials->pPrivateKey);

        if (sslStatus != 1)
        {
            print_openssl_errors();
            LogError(("SSL_CTX_use_PrivateKey failed to import client certificate private key."));
            return sslStatus;
        }
    }
    else
    {
        LogError(("Private key is NULL."));
        return -1;
    }

    /* Verify the private key matches the certificate. */
    sslStatus = SSL_CTX_check_private_key(pSslContext);

    if (sslStatus != 1)
    {
        // Print OpenSSL error to get detailed information
        unsigned long error_code = ERR_get_error();
        ERR_error_string_n(error_code, error_buf, sizeof(error_buf));

        LogError(("Private key/certificate mismatch. Details: %s", error_buf));

        return sslStatus; // Return the error code immediately
    }

    return sslStatus;
}

/*-----------------------------------------------------------*/

static void setOptionalConfigurations( SSL * pSsl,
                                       const OpensslCredentials_t * pOpensslCredentials )
{
    int32_t sslStatus = -1;
    int16_t readBufferLength = 0;

    assert( pSsl != NULL );
    assert( pOpensslCredentials != NULL );

    /* Set TLS ALPN if requested. */
    if( ( pOpensslCredentials->pAlpnProtos != NULL ) &&
        ( pOpensslCredentials->alpnProtosLen > 0U ) )
    {
        LogDebug( ( "Setting ALPN protos." ) );
        sslStatus = SSL_set_alpn_protos(
            pSsl, ( const uint8_t * ) pOpensslCredentials->pAlpnProtos,
            ( uint32_t ) pOpensslCredentials->alpnProtosLen );

        if( sslStatus != 0 )
        {
            LogError( ( "SSL_set_alpn_protos failed to set ALPN protos. %s",
                        pOpensslCredentials->pAlpnProtos ) );
        }
    }

    /* Set TLS MFLN if requested. */
    if( pOpensslCredentials->maxFragmentLength > 0U )
    {
        LogDebug( ( "Setting max send fragment length %u.",
                    pOpensslCredentials->maxFragmentLength ) );

        /* Set the maximum send fragment length. */

        /* MISRA Directive 4.6 flags the following line for using basic
         * numerical type long. This directive is suppressed because openssl
         * function #SSL_set_max_send_fragment expects a length argument
         * type of long. */
        /* coverity[misra_c_2012_directive_4_6_violation] */
        sslStatus = ( int32_t ) SSL_set_max_send_fragment(
            pSsl, ( long ) pOpensslCredentials->maxFragmentLength );

        if( sslStatus != 1 )
        {
            LogError( ( "Failed to set max send fragment length %u.",
                        pOpensslCredentials->maxFragmentLength ) );
        }
        else
        {
            readBufferLength = ( int16_t ) pOpensslCredentials->maxFragmentLength +
                               SSL3_RT_MAX_ENCRYPTED_OVERHEAD;

            /* Change the size of the read buffer to match the
             * maximum fragment length + some extra bytes for overhead. */
            SSL_set_default_read_buffer_len( pSsl, ( size_t ) readBufferLength );
        }
    }

    /* Enable SNI if requested. */
    if( pOpensslCredentials->sniHostName != NULL )
    {
        LogDebug(
            ( "Setting server name %s for SNI.", pOpensslCredentials->sniHostName ) );

        /* MISRA Rule 11.8 flags the following line for removing the const
         * qualifier from the pointed to type. This rule is suppressed because
         * openssl implementation of #SSL_set_tlsext_host_name internally casts
         * the pointer to a string literal to a `void *` pointer. */
        /* coverity[misra_c_2012_rule_11_8_violation] */
        sslStatus = ( int32_t ) SSL_set_tlsext_host_name(
            pSsl, pOpensslCredentials->sniHostName );

        if( sslStatus != 1 )
        {
            LogError( ( "Failed to set server name %s for SNI.",
                        pOpensslCredentials->sniHostName ) );
        }
    }
}
/*-----------------------------------------------------------*/

static int32_t isValidNetworkContext( const NetworkContext_t * pNetworkContext )
{
    int32_t isValid = 0;

    if( ( pNetworkContext == NULL ) || ( pNetworkContext->pParams == NULL ) )
    {
        LogError( ( "Parameter check failed: pNetworkContext is NULL." ) );
    }
    else
    {
        isValid = 1;
    }

    return isValid;
}
/*-----------------------------------------------------------*/

/* Function to print OpenSSL errors */
static void print_openssl_errors(void) {
    unsigned long err_code;
    while ((err_code = ERR_get_error()) != 0) {
        char err_buf[256];
        ERR_error_string_n(err_code, err_buf, sizeof(err_buf));
        printf("OpenSSL error: %s\n", err_buf);
    }
}

static EVP_PKEY *load_private_key_from_hsm(const char *keyIdentifier) {

    pctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (pctx == NULL) {
        LogError(("Failed to create EVP_PKEY_CTX with TrustM provider\n"));
        print_openssl_errors();
        return NULL;
    }
    
    if (EVP_PKEY_fromdata_init(pctx) <= 0) {
        LogError(("Failed to initialize EVP_PKEY context\n"));
        print_openssl_errors();
        EVP_PKEY_CTX_free(pctx);
        return NULL;
    }

    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string("key", (char *)keyIdentifier, 0),
        OSSL_PARAM_construct_end()
    };

    if (EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_KEYPAIR, params) <= 0) {
        LogError(("Failed to load key from TrustM\n"));
        print_openssl_errors();
        EVP_PKEY_CTX_free(pctx);
        return NULL;
    }

    EVP_PKEY_CTX_free(pctx);
    return pkey;
}

static EVP_PKEY *load_public_key_from_cert(const char *cert_path) {
    FILE *fp = fopen(cert_path, "r");
    if (fp == NULL) {
        perror("Unable to open certificate file");
        return NULL;
    }

    X509 *cert = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);

    if (cert == NULL) {
        LogError(("Unable to parse certificate\n"));
        return NULL;
    }
    
    EVP_PKEY *pkey = X509_get_pubkey(cert);
    X509_free(cert);

    return pkey;
}

static int compare_public_keys(EVP_PKEY *key1, EVP_PKEY *key2) {
    if (EVP_PKEY_cmp(key1, key2) == 1) {
        return 1; // Keys match
    } else {
        return 0; // Keys do not match
    }
}

OpensslStatus_t Openssl_Connect(NetworkContext_t *pNetworkContext,
                                const ServerInfo_t *pServerInfo,
                                const OpensslCredentials_t *pOpensslCredentials,
                                uint32_t sendTimeoutMs,
                                uint32_t recvTimeoutMs) {
    SSL_CTX *pSslContext = NULL;
    EVP_PKEY *pkey = NULL;
    EVP_PKEY *cert_pubkey = NULL;
    OpensslStatus_t returnStatus = OPENSSL_SUCCESS;
    OpensslParams_t *pOpensslParams = NULL;
    SocketStatus_t socketStatus = SOCKETS_INVALID_PARAMETER;
    uint8_t sslObjectCreated = 0u;

    if (!initialize_trustm_provider()) {
        LogError(("Failed to initialize TrustM provider\n"));
        return OPENSSL_API_ERROR;
    }

    pkey = load_private_key_from_hsm(PRIVATE_KEYSLOT);
    if (pkey == NULL) {
        LogError(("Failed to load private key from TrustM\n"));
        return OPENSSL_API_ERROR;
    }

    LogDebug(("Client certificate path: %s", pOpensslCredentials->pClientCertPath));
    cert_pubkey = load_public_key_from_cert(pOpensslCredentials->pClientCertPath);
    if (cert_pubkey == NULL) {
        LogError(("Failed to load public key from certificate\n"));
        EVP_PKEY_free(pkey);
        return OPENSSL_API_ERROR;
    }

    if (!compare_public_keys(cert_pubkey, pkey)) {
        LogError(("Public key from certificate does not match private key from TrustM\n"));
        EVP_PKEY_free(cert_pubkey);
        EVP_PKEY_free(pkey);
        return OPENSSL_API_ERROR;
    }

    // Create a new SSL context
    pSslContext = SSL_CTX_new_ex(libctx, propq, TLS_client_method());
    if (pSslContext == NULL) {
        LogError(("Creation of a new SSL_CTX object failed.\n"));
        ERR_print_errors_fp(stderr);
        returnStatus = OPENSSL_API_ERROR;
    }

    pOpensslCredentials.pPrivateKey = pkey;

    // Set the client certificate if provided
     if (pOpensslCredentials.pClientCertPath != NULL) {
        LogDebug(("Setting client certificate path: %s", pOpensslCredentials->pClientCertPath));
        if (SSL_CTX_use_certificate_file(pSslContext, pOpensslCredentials->pClientCertPath, SSL_FILETYPE_PEM) != 1) {
            print_openssl_errors();
            LogError(("SSL_CTX_use_certificate_file failed to load client certificate.\n"));
            return OPENSSL_API_ERROR;
        }
    } else {
        LogError(("Client certificate path is NULL.\n"));
        return OPENSSL_API_ERROR;
    }

    if (SSL_CTX_use_PrivateKey(pSslContext, pkey) != 1) {
        print_openssl_errors();
        LogError(("Error setting private key to SSL context.\n"));
        return OPENSSL_API_ERROR;
    }

    if (SSL_CTX_check_private_key(pSslContext) != 1) {
        print_openssl_errors();
        LogError(("Private key/certificate mismatch.\n"));
        return OPENSSL_API_ERROR;
    }

    if (returnStatus == OPENSSL_SUCCESS) {
        pOpensslParams = pNetworkContext->pParams;
        socketStatus = Sockets_Connect(&pOpensslParams->socketDescriptor,
                                       pServerInfo, sendTimeoutMs, recvTimeoutMs);
        returnStatus = convertToOpensslStatus(socketStatus);
    }

    if (returnStatus == OPENSSL_SUCCESS) {
        if (setCredentials(pSslContext, &pOpensslCredentials) != 1) {
            LogError(("Setting up credentials failed.\n"));
            returnStatus = OPENSSL_INVALID_CREDENTIALS;
        }
    }

    if (returnStatus == OPENSSL_SUCCESS) {
        pOpensslParams->pSsl = SSL_new(pSslContext);
        if (pOpensslParams->pSsl == NULL) {
            LogError(("SSL_new failed to create a new SSL context.\n"));
            returnStatus = OPENSSL_API_ERROR;
        } else {
            sslObjectCreated = 1u;
        }
    }

    if (returnStatus == OPENSSL_SUCCESS) {
        returnStatus = tlsHandshake(pServerInfo, pOpensslParams, &pOpensslCredentials);
    }

    if (pSslContext != NULL) {
    SSL_CTX_free(pSslContext);
    pSslContext = NULL;
    }
    
    if (pkey != NULL) {
        EVP_PKEY_free(pkey);
    }
    
    if (cert_pubkey != NULL) {
        EVP_PKEY_free(cert_pubkey);
    }

    if (returnStatus != OPENSSL_SUCCESS && sslObjectCreated == 1u) {
        SSL_free(pOpensslParams->pSsl);
        pOpensslParams->pSsl = NULL;
    }

    if (returnStatus != OPENSSL_SUCCESS) {
        LogError(("Failed to establish a TLS connection."));
    } else {
        LogDebug(("Established a TLS connection."));
    }

    return returnStatus;
}


/*-----------------------------------------------------------*/

OpensslStatus_t Openssl_Disconnect( const NetworkContext_t * pNetworkContext )
{
    OpensslParams_t * pOpensslParams = NULL;
    SocketStatus_t socketStatus = SOCKETS_INVALID_PARAMETER;

    if( ( pNetworkContext == NULL ) || ( pNetworkContext->pParams == NULL ) )
    {
        /* No need to update the status here. The socket status
         * SOCKETS_INVALID_PARAMETER will be converted to openssl
         * status OPENSSL_INVALID_PARAMETER before returning from this
         * function. */
        LogError( ( "Parameter check failed: pNetworkContext is NULL." ) );
    }
    else
    {
        pOpensslParams = pNetworkContext->pParams;

        if( pOpensslParams->pSsl != NULL )
        {
            /* SSL shutdown should be called twice: once to send "close notify" and
             * once more to receive the peer's "close notify". */
            if( SSL_shutdown( pOpensslParams->pSsl ) == 0 )
            {
                ( void ) SSL_shutdown( pOpensslParams->pSsl );
            }

            SSL_free( pOpensslParams->pSsl );
            pOpensslParams->pSsl = NULL;
        }

        /* Tear down the socket connection, pNetworkContext != NULL here. */
        socketStatus = Sockets_Disconnect( pOpensslParams->socketDescriptor );
    }

    return convertToOpensslStatus( socketStatus );
}
/*-----------------------------------------------------------*/

/* MISRA Rule 8.13 flags the following line for not using the const qualifier
 * on `pNetworkContext`. Indeed, the object pointed by it is not modified
 * by OpenSSL, but other implementations of `TransportRecv_t` may do so. */
int32_t Openssl_Recv( NetworkContext_t * pNetworkContext,
                      void * pBuffer,
                      size_t bytesToRecv )
{
    OpensslParams_t * pOpensslParams = NULL;
    int32_t bytesReceived = 0;

    if( !isValidNetworkContext( pNetworkContext ) ||
        ( pBuffer == NULL ) ||
        ( bytesToRecv == 0 ) )
    {
        LogError( ( "Parameter check failed: invalid input, pNetworkContext is invalid or pBuffer = %p, bytesToRecv = %lu", pBuffer, bytesToRecv ) );
        bytesReceived = -1;
    }
    else
    {
        int32_t pollStatus = 1, readStatus = 1, sslError = 0;
        uint8_t shouldRead = 0U;
        struct pollfd pollFds;
        pOpensslParams = pNetworkContext->pParams;

        /* Initialize the file descriptor.
         * #POLLPRI corresponds to high-priority data while #POLLIN corresponds
         * to any other data that may be read. */
        pollFds.events = POLLIN | POLLPRI;
        pollFds.revents = 0;
        /* Set the file descriptor for poll. */
        pollFds.fd = pOpensslParams->socketDescriptor;

        /* #SSL_pending returns a value > 0 if application data
         * from the last processed TLS record remains to be read.
         * This implementation will ALWAYS block when the number of bytes
         * requested is greater than 1. Otherwise, poll the socket first
         * as blocking may negatively impact performance by waiting for the
         * entire duration of the socket timeout even when no data is available. */
        if( ( bytesToRecv > 1 ) || ( SSL_pending( pOpensslParams->pSsl ) > 0 ) )
        {
            shouldRead = 1U;
        }
        else
        {
            /* Speculative read for the start of a payload.
             * Note: This is done to avoid blocking when no
             * data is available to be read from the socket. */
            pollStatus = poll( &pollFds, 1, 0 );
        }

        if( pollStatus < 0 )
        {
            bytesReceived = -1;
        }
        else if( pollStatus == 0 )
        {
            /* No data available to be read from the socket. */
            bytesReceived = 0;
        }
        else
        {
            shouldRead = 1U;
        }

        if( shouldRead == 1U )
        {
            /* Blocking SSL read of data.
             * Note: The TLS record may only be partially received or unprocessed,
             * so it is possible that no processed application data is returned
             * even though the socket has data available to be read. */
            readStatus = ( int32_t ) SSL_read( pOpensslParams->pSsl, pBuffer,
                                               ( int32_t ) bytesToRecv );

            /* Successfully read of application data. */
            if( readStatus > 0 )
            {
                bytesReceived = readStatus;
            }
        }

        /* Handle error return status if transport read did not succeed. */
        if( readStatus <= 0 )
        {
            sslError = SSL_get_error( pOpensslParams->pSsl, readStatus );

            if( sslError == SSL_ERROR_WANT_READ )
            {
                /* The OpenSSL documentation mentions that SSL_Read can provide a
                 * return code of SSL_ERROR_WANT_READ in blocking mode, if the SSL
                 * context is not configured with with the SSL_MODE_AUTO_RETRY. This
                 * error code means that the SSL_read() operation needs to be retried
                 * to complete the read operation. Thus, setting the return value of
                 * this function as zero to represent that no data was received from
                 * the network. */
                bytesReceived = 0;
            }
            else if (sslError == SSL_ERROR_SYSCALL) {
                LogError(("SSL_read failed with SSL_ERROR_SYSCALL\n"));
                print_openssl_errors();
                bytesReceived = -1;
            }
            else
            {
                LogError( ( "Failed to receive data over network: SSL_read failed: "
                            "ErrorStatus=%s.",
                            ERR_reason_error_string( sslError ) ) );

                /* The transport interface requires zero return code only when the
                 * receive operation can be retried to achieve success. Thus, convert
                 * a zero error code to a negative return value as this cannot be
                 * retried. */
                bytesReceived = -1;
            }
        }
    }

    return bytesReceived;
}
/*-----------------------------------------------------------*/

/* MISRA Rule 8.13 flags the following line for not using the const qualifier
 * on `pNetworkContext`. Indeed, the object pointed by it is not modified
 * by OpenSSL, but other implementations of `TransportSend_t` may do so. */
int32_t Openssl_Send( NetworkContext_t * pNetworkContext,
                      const void * pBuffer,
                      size_t bytesToSend )
{
    OpensslParams_t * pOpensslParams = NULL;
    int32_t bytesSent = 0;

    if( !isValidNetworkContext( pNetworkContext ) ||
        ( pBuffer == NULL ) ||
        ( bytesToSend == 0 ) )
    {
        LogError( ( "Parameter check failed: invalid input, pNetworkContext is invalid or pBuffer = %p, bytesToSend = %lu", pBuffer, bytesToSend ) );
        bytesSent = -1;
    }
    else
    {
        struct pollfd pollFds;
        int32_t pollStatus;

        pOpensslParams = pNetworkContext->pParams;

        /* Initialize the file descriptor. */
        pollFds.events = POLLOUT;
        pollFds.revents = 0;
        /* Set the file descriptor for poll. */
        pollFds.fd = pOpensslParams->socketDescriptor;

        /* `poll` checks if the socket is ready to send data.
         * Note: This is done to avoid blocking on SSL_write()
         * when TCP socket is not ready to accept more data for
         * network transmission (possibly due to a full TX buffer). */
        pollStatus = poll( &pollFds, 1, 0 );

        if( pollStatus > 0 )
        {
            /* SSL write of data. */
            bytesSent = ( int32_t ) SSL_write( pOpensslParams->pSsl, pBuffer,
                                               ( int32_t ) bytesToSend );

            if( bytesSent <= 0 )
            {
                LogError(
                    ( "Failed to send data over network: SSL_write of OpenSSL failed: "
                      "ErrorStatus=%s.",
                      ERR_reason_error_string( SSL_get_error( pOpensslParams->pSsl, bytesSent ) ) ) );

                /* As the SSL context is configured for blocking mode, the SSL_write()
                 * function does not return an SSL_ERROR_WANT_READ or
                 * SSL_ERROR_WANT_WRITE error code. The SSL_ERROR_WANT_READ and
                 * SSL_ERROR_WANT_WRITE error codes signify that the write operation can
                 * be retried. However, in the blocking mode, as the SSL_write()
                 * function does not return either of the error codes, we cannot retry
                 * the operation on failure, and thus, this function will never return a
                 * zero error code.
                 */

                /* The transport interface requires zero return code only when the send
                 * operation can be retried to achieve success. Thus, convert a zero
                 * error code to a negative return value as this cannot be retried. */
                if( bytesSent == 0 )
                {
                    bytesSent = -1;
                }
            }
        }
        else if( pollStatus < 0 )
        {
            /* An error occurred while polling. */
            LogError( ( "Unable to send TLS data on network: "
                        "An error occurred while checking availability of TCP socket %d.",
                        pOpensslParams->socketDescriptor ) );
            bytesSent = -1;
        }
        else
        {
            /* Socket is not available for sending data. Set return code for retrying send. */
            bytesSent = 0;
        }
    }

    return bytesSent;
}
/*-----------------------------------------------------------*/

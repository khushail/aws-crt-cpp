/*
 * Copyright 2010-2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
#include <aws/crt/Api.h>

#include <aws/crt/auth/Credentials.h>
#include <aws/crt/auth/Sigv4Signing.h>
#include <aws/crt/http/HttpRequestResponse.h>

#include <aws/auth/credentials.h>
#include <aws/http/request_response.h>

#include <aws/testing/aws_test_harness.h>

#include <condition_variable>
#include <mutex>

using namespace Aws::Crt;
using namespace Aws::Crt::Auth;
using namespace Aws::Crt::Http;

class SignWaiter
{
  public:
    SignWaiter() : m_lock(), m_signal(), m_done(false) {}

    void OnSigningComplete(const std::shared_ptr<Aws::Crt::Http::HttpRequest> &request, int errorCode)
    {
        std::unique_lock<std::mutex> lock(m_lock);
        m_done = true;
        m_signal.notify_one();
    }

    void Wait()
    {
        {
            std::unique_lock<std::mutex> lock(m_lock);
            m_signal.wait(lock, [this]() { return m_done == true; });
        }
    }

  private:
    std::mutex m_lock;
    std::condition_variable m_signal;
    bool m_done;
};

static std::shared_ptr<HttpRequest> s_MakeDummyRequest(Allocator *allocator)
{
    auto request = MakeShared<HttpRequest>(allocator);

    request->SetMethod(ByteCursor("GET"));
    request->SetPath(ByteCursor("http://www.test.com/mctest"));

    HttpHeader header;
    header.name = aws_byte_cursor_from_c_str("Host");
    header.value = aws_byte_cursor_from_c_str("www.test.com");

    request->AddHeader(header);

    auto bodyStream = Aws::Crt::MakeShared<std::stringstream>(allocator, "Something");
    request->SetBody(bodyStream);

    return request;
}

static std::shared_ptr<Credentials> s_MakeDummyCredentials(Allocator *allocator)
{
    return Aws::Crt::MakeShared<Credentials>(
        allocator, ByteCursor("access"), ByteCursor("secret"), ByteCursor("token"));
}

static std::shared_ptr<ICredentialsProvider> s_MakeAsyncStaticProvider(
    Allocator *allocator,
    const Aws::Crt::Io::ClientBootstrap &bootstrap)
{
    struct aws_credentials_provider_imds_options imds_options;
    AWS_ZERO_STRUCT(imds_options);
    imds_options.bootstrap = bootstrap.GetUnderlyingHandle();

    struct aws_credentials_provider *provider1 = aws_credentials_provider_new_imds(allocator, &imds_options);

    struct aws_credentials_provider *provider2 = aws_credentials_provider_new_static(
        allocator,
        aws_byte_cursor_from_c_str("access"),
        aws_byte_cursor_from_c_str("secret"),
        aws_byte_cursor_from_c_str("token"));

    struct aws_credentials_provider *providers[2] = {provider1, provider2};

    struct aws_credentials_provider_chain_options options;
    AWS_ZERO_STRUCT(options);
    options.providers = providers;
    options.provider_count = 2;

    struct aws_credentials_provider *provider_chain = aws_credentials_provider_new_chain(allocator, &options);
    aws_credentials_provider_release(provider1);
    aws_credentials_provider_release(provider2);
    if (provider_chain == NULL)
    {
        return nullptr;
    }

    return Aws::Crt::MakeShared<CredentialsProvider>(allocator, provider_chain, allocator);
}

static int s_Sigv4SignerTestCreateDestroy(struct aws_allocator *allocator, void *ctx)
{
    (void)ctx;
    Aws::Crt::ApiHandle apiHandle(allocator);

    {
        auto signer = Aws::Crt::MakeShared<Sigv4HttpRequestSigner>(allocator, allocator);
    }

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(Sigv4SignerTestCreateDestroy, s_Sigv4SignerTestCreateDestroy)

static int s_Sigv4SignerTestSimple(struct aws_allocator *allocator, void *ctx)
{
    (void)ctx;
    Aws::Crt::ApiHandle apiHandle(allocator);

    {
        auto signer = Aws::Crt::MakeShared<Sigv4HttpRequestSigner>(allocator, allocator);
        auto request = s_MakeDummyRequest(allocator);
        auto credentials = s_MakeDummyCredentials(allocator);

        auto config = Aws::Crt::MakeShared<AwsSigningConfig>(allocator, allocator);
        config->SetCredentials(credentials);
        config->SetDate(Aws::Crt::DateTime());
        config->SetRegion(ByteCursor("test"));
        config->SetService(ByteCursor("service"));

        signer->SignRequest(*request, config.get());
    }

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(Sigv4SignerTestSimple, s_Sigv4SignerTestSimple)

static int s_Sigv4SigningPipelineTestCreateDestroy(struct aws_allocator *allocator, void *ctx)
{
    (void)ctx;
    Aws::Crt::ApiHandle apiHandle(allocator);

    {
        Aws::Crt::Io::EventLoopGroup eventLoopGroup(0, allocator);
        ASSERT_TRUE(eventLoopGroup);

        Aws::Crt::Io::DefaultHostResolver defaultHostResolver(eventLoopGroup, 8, 30, allocator);
        ASSERT_TRUE(defaultHostResolver);

        Aws::Crt::Io::ClientBootstrap clientBootstrap(eventLoopGroup, defaultHostResolver, allocator);
        ASSERT_TRUE(clientBootstrap);

        CredentialsProviderChainDefaultConfig config;
        config.m_bootstrap = &clientBootstrap;

        auto provider = Aws::Crt::Auth::CredentialsProvider::CreateCredentialsProviderChainDefault(config);

        auto pipeline = Aws::Crt::MakeShared<Sigv4HttpRequestSigningPipeline>(allocator, provider, allocator);
    }

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(Sigv4SigningPipelineTestCreateDestroy, s_Sigv4SigningPipelineTestCreateDestroy)

static int s_Sigv4SigningPipelineTestSimple(struct aws_allocator *allocator, void *ctx)
{
    (void)ctx;
    Aws::Crt::ApiHandle apiHandle(allocator);

    {
        Aws::Crt::Io::EventLoopGroup eventLoopGroup(0, allocator);
        ASSERT_TRUE(eventLoopGroup);

        Aws::Crt::Io::DefaultHostResolver defaultHostResolver(eventLoopGroup, 8, 30, allocator);
        ASSERT_TRUE(defaultHostResolver);

        Aws::Crt::Io::ClientBootstrap clientBootstrap(eventLoopGroup, defaultHostResolver, allocator);
        ASSERT_TRUE(clientBootstrap);

        CredentialsProviderChainDefaultConfig config;
        config.m_bootstrap = &clientBootstrap;

        auto provider = s_MakeAsyncStaticProvider(allocator, clientBootstrap);

        auto pipeline = Aws::Crt::MakeShared<Sigv4HttpRequestSigningPipeline>(allocator, provider, allocator);

        auto request = s_MakeDummyRequest(allocator);

        auto signingConfig = Aws::Crt::MakeShared<AwsSigningConfig>(allocator, allocator);
        signingConfig->SetDate(Aws::Crt::DateTime());
        signingConfig->SetRegion(ByteCursor("test"));
        signingConfig->SetService(ByteCursor("service"));

        SignWaiter waiter;

        pipeline->SignRequest(
            request, signingConfig, [&](const std::shared_ptr<Aws::Crt::Http::HttpRequest> &request, int errorCode) {
                waiter.OnSigningComplete(request, errorCode);
            });
        waiter.Wait();
    }

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(Sigv4SigningPipelineTestSimple, s_Sigv4SigningPipelineTestSimple)
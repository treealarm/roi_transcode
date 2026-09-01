// What a component may claim about itself, and what the registry refuses -- TZ v4.2 §26.4.
//
// §26.4 names three checks by hand -- abi_version, descriptor_size and the backend
// identifier -- and forbids a duplicate registration being resolved by silent override.
// This is where each of those is a failing call rather than a paragraph.
//
// The descriptors below acquire a real backend rather than a stub. A stub would have to
// implement IEncoderBackend whole, and every check here is about the descriptor wrapped
// around a backend, not about the backend: the interesting cases are a descriptor that
// lies, and there is nothing to lie about without something true to compare against.

#include "sve/backend_plugin.h"
#include "sve/backend_registry.h"
#include "sve/encoder_backend.h"

#include <cstdio>
#include <cstdlib>

using namespace sve;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "backend_plugin_test: %s\n", what);
        ++failures;
    }
}

/// The one backend this test build is certain to have: libx264 is a direct component and
/// needs no device to exist, only to encode.
const IEncoderBackend* acquire_real() noexcept
{
    return default_backend_registry().Find(Backend::X264Direct);
}

const IEncoderBackend* acquire_nothing() noexcept
{
    return nullptr;
}

constexpr SveBackendPluginDescriptorV1 good()
{
    return SveBackendPluginDescriptorV1{
        .abi_version = kBackendPluginAbiVersion,
        .descriptor_size = sizeof(SveBackendPluginDescriptorV1),
        .backend_id = static_cast<int32_t>(Backend::X264Direct),
        .acquire = &acquire_real,
    };
}

/// The message matters as much as the refusal: §50's rule is that a degradation nobody can
/// read is a silent one, and "InvalidArgument" alone does not say which claim was wrong.
void refused(const SveBackendPluginDescriptorV1& descriptor, const char* needle,
    const char* what)
{
    BackendRegistry registry;
    const Status s = registry.Register(descriptor);
    check(s.code() == StatusCode::InvalidArgument, what);
    check(s.message().find(needle) != std::string::npos, what);
    check(registry.kinds().empty(), what);
}

void a_well_formed_descriptor_registers()
{
    BackendRegistry registry;
    check(registry.Register(good()).ok(), "a well-formed descriptor was refused");
    check(registry.kinds().size() == 1, "the backend did not arrive");
    check(registry.Find(Backend::X264Direct) != nullptr, "the backend is not findable");
}

void a_version_this_build_does_not_know_is_refused()
{
    SveBackendPluginDescriptorV1 d = good();
    d.abi_version = kBackendPluginAbiVersion + 1;
    refused(d, "abi version", "a future abi version was accepted");

    d = good();
    d.abi_version = 0;
    refused(d, "abi version", "abi version zero was accepted");
}

void a_descriptor_of_the_wrong_size_is_refused()
{
    // What a component rebuilt against a different header looks like from here. The
    // version can agree while the layout does not, which is why §26.4 asks for both.
    SveBackendPluginDescriptorV1 d = good();
    d.descriptor_size = sizeof(SveBackendPluginDescriptorV1) - 1;
    refused(d, "bytes", "a short descriptor was accepted");

    d = good();
    d.descriptor_size = sizeof(SveBackendPluginDescriptorV1) + 8;
    refused(d, "bytes", "a long descriptor was accepted");
}

void a_descriptor_that_cannot_produce_a_backend_is_refused()
{
    SveBackendPluginDescriptorV1 d = good();
    d.acquire = nullptr;
    refused(d, "acquire", "a descriptor with no acquire function was accepted");

    d = good();
    d.acquire = &acquire_nothing;
    refused(d, "acquired no backend", "a descriptor acquiring nothing was accepted");
}

void a_descriptor_naming_one_backend_and_acquiring_another_is_refused()
{
    // The check that is not about corruption but about a mistake a component can make on
    // its own: copy a descriptor, change the acquire function, forget the identifier. It
    // would register libx264 under the name of libx265, and every capability answer after
    // that would be about the wrong backend.
    SveBackendPluginDescriptorV1 d = good();
    d.backend_id = static_cast<int32_t>(Backend::X265Direct);
    refused(d, "acquired", "a descriptor with a mismatched identifier was accepted");
}

void the_same_identifier_twice_is_refused()
{
    // §26.4 in as many words: a component linked in and its plugin loaded as well is the
    // case this exists for, and choosing between them would be choosing at random.
    BackendRegistry registry;
    check(registry.Register(good()).ok(), "the first registration failed");
    const Status second = registry.Register(good());
    check(second.code() == StatusCode::InvalidArgument, "a duplicate was accepted");
    check(second.message().find("already registered") != std::string::npos,
        "the duplicate refusal does not say so");
    check(registry.kinds().size() == 1, "the duplicate was registered anyway");
}

} // namespace

int main()
{
    a_well_formed_descriptor_registers();
    a_version_this_build_does_not_know_is_refused();
    a_descriptor_of_the_wrong_size_is_refused();
    a_descriptor_that_cannot_produce_a_backend_is_refused();
    a_descriptor_naming_one_backend_and_acquiring_another_is_refused();
    the_same_identifier_twice_is_refused();

    if (failures != 0) {
        std::fprintf(stderr, "backend_plugin_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::puts("backend_plugin_test ok");
    return 0;
}

// Plugins that exist to be loaded and to be refused -- TZ v4.2 §26.4.
//
// Four modules out of one source, because what separates them is exactly the thing under
// test: what a descriptor claims. Building them from one file keeps the difference visible
// instead of spread across four near-identical files that would drift apart.
//
//   SVE_TEST_PLUGIN_GOOD      a descriptor this build accepts
//   SVE_TEST_PLUGIN_DUP       the same identifier again, to be refused as a duplicate
//   SVE_TEST_PLUGIN_BAD_ABI   a version from a build that does not exist
//   SVE_TEST_PLUGIN_NO_ENTRY  a module with no entry point at all
//
// The backend behind the good ones answers "not on this machine" to everything. The loader
// registers backends; it never encodes with one, and a stub that pretended to would be
// testing something this file is not about.

#include "sve/backend_plugin.h"
#include "sve/encoder_backend.h"

#include <string>

namespace sve {
namespace {

class StubBackend final : public IEncoderBackend {
public:
    // NvidiaNvenc because no build of this project provides it as a component, so a test
    // registry can take it without colliding with anything real.
    Backend kind() const override { return Backend::NvidiaNvenc; }

    EncoderCapabilities GetCapabilities(const CapabilityQuery&) const override
    {
        return EncoderCapabilities{};
    }

    Result<SessionPtr> Configure(const BackendConfig&) const override
    {
        // backend_name is defined in the core and nowhere in this module, so this line is
        // what makes the module actually depend on the host at load time. That is the
        // arrangement under test as much as the descriptor is: the plugin carries no copy
        // of the core, and a host that did not export its own symbols fails at dlopen with
        // a reason rather than crashing later.
        return Status::DeviceUnavailable(
            std::string(backend_name(kind())) + " encodes nothing in a test");
    }

    Result<FrameBufferPtr> CreateFrame(BackendSession&, MemoryType) const override
    {
        return Status::Unsupported("the test plugin allocates nothing");
    }

    Status SubmitFrame(BackendSession&, IFrameBuffer&,
        const BackendFrameControl&) const override
    {
        return Status::Unsupported("the test plugin encodes nothing");
    }

    Result<EncodedPacket> ReceivePacket(BackendSession&) const override
    {
        return Status::EndOfStream();
    }

    Status Flush(BackendSession&) const override { return Status::Ok(); }

    Result<CodecParameters> GetCodecParameters(BackendSession&) const override
    {
        return Status::Unsupported("the test plugin has no stream");
    }

    BackendStatistics Statistics(const BackendSession&) const override
    {
        return BackendStatistics{};
    }
};

#if !defined(SVE_TEST_PLUGIN_NO_ENTRY)

const IEncoderBackend* acquire_stub() noexcept
{
    static const StubBackend instance;
    return &instance;
}

#if defined(SVE_TEST_PLUGIN_BAD_ABI)
constexpr uint32_t kAbi = kBackendPluginAbiVersion + 1000;
#else
constexpr uint32_t kAbi = kBackendPluginAbiVersion;
#endif

constexpr SveBackendPluginDescriptorV1 kDescriptor{
    .abi_version = kAbi,
    .descriptor_size = sizeof(SveBackendPluginDescriptorV1),
    .backend_id = static_cast<int32_t>(Backend::NvidiaNvenc),
    .acquire = &acquire_stub,
};

#endif // !SVE_TEST_PLUGIN_NO_ENTRY

} // namespace

#if !defined(SVE_TEST_PLUGIN_NO_ENTRY)
extern "C" SVE_BACKEND_EXPORT
const SveBackendPluginDescriptorV1* sve_backend_plugin_v1() noexcept
{
    return &kDescriptor;
}
#endif

} // namespace sve

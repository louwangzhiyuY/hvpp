#include "hypervisor.h"
#include "config.h"

#include "ia32/cpuid/cpuid_eax_01.h"
#include "lib/assert.h"
#include "lib/log.h"
#include "lib/mm.h"
#include "lib/mp.h"

#include <scoped_allocator>

namespace hvpp::hypervisor
{
  namespace detail
  {
    static
    bool
    check_cpu_features(
      void
      ) noexcept
    {
      cpuid_eax_01 cpuid_info;
      ia32_asm_cpuid(cpuid_info.cpu_info, 1);
      if (!cpuid_info.feature_information_ecx.virtual_machine_extensions)
      {
        return false;
      }

      auto cr4 = read<cr4_t>();
      if (cr4.vmx_enable)
      {
        return false;
      }

      auto vmx_basic = msr::read<msr::vmx_basic_t>();
      if (
          vmx_basic.vmcs_size_in_bytes > page_size ||
          vmx_basic.memory_type != uint64_t(memory_type::write_back) ||
         !vmx_basic.true_controls
        )
      {
        return false;
      }

      auto vmx_ept_vpid_cap = msr::read<msr::vmx_ept_vpid_cap_t>();
      if (
        !vmx_ept_vpid_cap.page_walk_length_4      ||
        !vmx_ept_vpid_cap.memory_type_write_back  ||
        !vmx_ept_vpid_cap.invept                  ||
        !vmx_ept_vpid_cap.invept_all_contexts     ||
        !vmx_ept_vpid_cap.execute_only_pages      ||
        !vmx_ept_vpid_cap.pde_2mb_pages
        )
      {
        return false;
      }

      return true;
    }
  }

  struct global_t
  {
    vcpu_t* vcpu_list;
    bool    started;
  };

  global_t global;

  auto start(vmexit_handler& handler) noexcept -> error_code_t
  {
    //
    // If hypervisor is already running,
    // don't do anything.
    //
    hvpp_assert(!global.started);
    if (global.started)
    {
      return make_error_code_t(std::errc::operation_not_permitted);
    }

    //
    // Create array of VCPUs.
    // Note that since
    //   - vcpu_t is not default-constructible
    //   - operator new[] doesn't support constructing objects
    //     with parameters
    // ... we have to construct this array "placement new".
    //
    hvpp_assert(global.vcpu_list == nullptr);

    global.vcpu_list = reinterpret_cast<vcpu_t*>(operator new[](sizeof(vcpu_t) * mp::cpu_count()));
    if (!global.vcpu_list)
    {
      return make_error_code_t(std::errc::not_enough_memory);
    }

    //
    // Construct each vcpu_t object as `vcpu_t(handler)'.
    //
    std::for_each_n(global.vcpu_list, mp::cpu_count(),
      [&](vcpu_t& vp) {
        ::new (static_cast<void*>(std::addressof(vp)))
          vcpu_t(handler);
      });

    //
    // Check that CPU supports all required features to
    // run this hypervisor.
    // Note that this check is performed only on current CPU
    // and assumes all CPUs are symmetrical.
    //
    if (!detail::check_cpu_features())
    {
      return make_error_code_t(std::errc::not_supported);
    }

    //
    // Start virtualization on all CPUs.
    // TODO:
    //   - error handling
    //   - create new error_category for VMX errors?
    //
    mp::ipi_call([]() {
      mm::allocator_guard _;

      auto idx = mp::cpu_index();
      global.vcpu_list[idx].launch();
    });

    //
    // Signalize that hypervisor has started.
    //
    global.started = true;

    return error_code_t{};
  }

  void stop() noexcept
  {
    //
    // If hypervisor is already stopped,
    // don't do anything.
    //
    hvpp_assert(global.started);
    if (!global.started)
    {
      return;
    }

    //
    // Stop virtualization on all CPUs.
    //
    mp::ipi_call([]() {
      mm::allocator_guard _;

      auto idx = mp::cpu_index();
      global.vcpu_list[idx].terminate(); // #TODO !!!!
    });

    //
    // Destroy array of VCPUs.
    //
    delete[] global.vcpu_list;
    global.vcpu_list = nullptr;

    //
    // Signalize that hypervisor has stopped.
    //
    global.started = false;
  }

  bool is_started() noexcept
  {
    return global.started;
  }
}

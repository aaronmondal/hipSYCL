/*
 * This file is part of hipSYCL, a SYCL implementation based on CUDA/HIP
 *
 * Copyright (c) 2018 Aksel Alpay
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef HIPSYCL_LOCAL_MEM_ALLOCATOR_HPP
#define HIPSYCL_LOCAL_MEM_ALLOCATOR_HPP

#include "hipSYCL/sycl/libkernel/backend.hpp"
#include "hipSYCL/sycl/exception.hpp"

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <array>

namespace hipsycl {
namespace sycl {

class device;

namespace detail {


class local_memory_allocator
{
public:
  using address = size_t;
  using smallest_type = int;

  // ToDo: Query max shared memory of device and check when allocating
  local_memory_allocator()
    : _num_allocated_bytes{0}
  {}

  template<class T>
  address alloc(size_t num_elements)
  {
    size_t num_bytes = num_elements * sizeof(T);

    size_t alignment = get_alignment<T>();

    size_t start_byte =
        alignment * ((get_allocation_size() + alignment - 1) / alignment);

    address start_address = start_byte;
    address end_address = start_address + num_bytes;

    _num_allocated_bytes = end_address;

    return start_address;
  }

  size_t get_allocation_size() const
  {
    return _num_allocated_bytes;
  }

  template<class T>
  size_t get_alignment() const
  {
    size_t alignment = std::max(sizeof(smallest_type), sizeof(T));
    // If T is not a multiple of an int in size (i.e, no power of 2
    // greater than 4), it is likely some weird data structure and not a vector
    // - in this case it's probably sufficient to align it by 32 bits.
    // ToDo: Check precise alignment rules for shared memory in CUDA and HIP
    if(sizeof(T) % sizeof(smallest_type) != 0)
      alignment = sizeof(smallest_type);

    return alignment;
  }
private:
  size_t _num_allocated_bytes;
};

enum class host_local_memory_origin { hipcpu, custom_threadprivate };


/// Manages local memory on host device.
/// Assumptions:
/// a) Only one host kernel is active at any give time (this is enforced by hipCPU's
///    kernel launch implementation)
/// b) Two modes of allocations are supported:
///    1) Just relying on whatever hipCPU allocates
///    as local memory. This will be one block of memory under the assumption
///    that only one work group is active at any given time.
///    2) Allocating a threadprivate memory region under the assumption that
///    there is one thread per work group.
///    For case 1), request/release pair must be called inside the kernel function, to
///    guarantee that the hipCPU block execution context which provides local memory exists.
///    For case 2), the request/release pair must be called inside the #pragma omp parallel block.
class host_local_memory
{
public:
  static void request_from_threadprivate_pool(size_t num_bytes)
  {
    alloc_threadprivate(num_bytes);
  }

  static void release()
  {
    release_memory();
  }

  static char* get_ptr()
  {
    return _local_mem;
  }

private:

  static void release_memory() {
    if (_local_mem != nullptr && _local_mem != &(_static_local_mem[0]) &&
        _origin != host_local_memory_origin::hipcpu)
      delete[] _local_mem;

    _local_mem = nullptr;
  }

  static void alloc_threadprivate(size_t num_bytes) {
    release_memory();

    _origin = host_local_memory_origin::custom_threadprivate;

    if(num_bytes <= _max_static_local_mem_size)
      _local_mem = &(_static_local_mem[0]);
    else
    {
      if(num_bytes > 0)
        _local_mem = new char [num_bytes];
    }
  }


  // By default we offer 32KB local memory per work group,
  // for more local memory we go to the heap.
  static constexpr size_t _max_static_local_mem_size = 1024*32;
  inline static char* _local_mem;

  alignas(sizeof(double) * 16) inline static char _static_local_mem
      [_max_static_local_mem_size];

  inline static host_local_memory_origin _origin;
#ifdef _OPENMP
  #pragma omp threadprivate(_local_mem)
  #pragma omp threadprivate(_static_local_mem)
  #pragma omp threadprivate(_origin)
#endif
};

HIPSYCL_KERNEL_TARGET
inline void* hiplike_dynamic_local_memory() {
  __hipsycl_if_target_cuda(
    extern __shared__ int local_mem [];
    return static_cast<void*>(local_mem);
  );
  __hipsycl_if_target_hip(
    return __amdgcn_get_dynamicgroupbaseptr();
  );
  __hipsycl_if_target_host(
    assert(false && "this function should only be called on device");
    return nullptr;
  );
}

class local_memory
{
public:
  using address = local_memory_allocator::address;

  template<class T>
  HIPSYCL_KERNEL_TARGET
  static T* get_ptr(const address addr)
  {
    __hipsycl_if_target_device(
      return reinterpret_cast<T *>(
        reinterpret_cast<char *>(hiplike_dynamic_local_memory()) + addr);
    );
    __hipsycl_if_target_host(
      return reinterpret_cast<T*>(host_local_memory::get_ptr() + addr);
    );
  }
};

}
}
}

#endif

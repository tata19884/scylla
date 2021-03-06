/*
 * Copyright (C) 2018 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <seastar/core/reactor.hh>
#include <seastar/core/print.hh>

#include "reader_concurrency_semaphore.hh"
#include "utils/exceptions.hh"


reader_permit::impl::impl(reader_concurrency_semaphore& semaphore, reader_resources base_cost) : semaphore(semaphore), base_cost(base_cost) {
}

reader_permit::impl::~impl() {
    semaphore.signal(base_cost);
}

reader_permit::memory_units::memory_units(reader_concurrency_semaphore* semaphore, ssize_t memory) noexcept
        : _semaphore(semaphore), _memory(memory) {
    if (_semaphore && _memory) {
        _semaphore->consume_memory(_memory);
    }
}

reader_permit::memory_units::memory_units(memory_units&& o) noexcept
    : _semaphore(std::exchange(o._semaphore, nullptr))
    , _memory(std::exchange(o._memory, 0)) {
}

reader_permit::memory_units::~memory_units() {
    reset();
}

reader_permit::memory_units& reader_permit::memory_units::operator=(memory_units&& o) noexcept {
    if (&o == this) {
        return *this;
    }
    reset();
    _semaphore = std::exchange(o._semaphore, nullptr);
    _memory = std::exchange(o._memory, 0);
    return *this;
}

void reader_permit::memory_units::reset(size_t memory) {
    if (_semaphore) {
        _semaphore->consume_memory(memory);
        _semaphore->signal_memory(_memory);
    }
    _memory = memory;
}

reader_permit::reader_permit(reader_concurrency_semaphore& semaphore, reader_resources base_cost)
    : _impl(make_lw_shared<reader_permit::impl>(semaphore, base_cost)) {
}

reader_permit::memory_units reader_permit::get_memory_units(size_t memory) {
    return memory_units(_impl ? &_impl->semaphore : nullptr, memory);
}

void reader_permit::release() {
    _impl->semaphore.signal(_impl->base_cost);
    _impl->base_cost = {};
}

reader_permit no_reader_permit() {
    return reader_permit{};
}

void reader_concurrency_semaphore::signal(const resources& r) noexcept {
    _resources += r;
    while (!_wait_list.empty() && has_available_units(_wait_list.front().res)) {
        auto& x = _wait_list.front();
        _resources -= x.res;
        try {
            x.pr.set_value(reader_permit(*this, x.res));
        } catch (...) {
            x.pr.set_exception(std::current_exception());
        }
        _wait_list.pop_front();
    }
}

reader_concurrency_semaphore::inactive_read_handle reader_concurrency_semaphore::register_inactive_read(std::unique_ptr<inactive_read> ir) {
    // Implies _inactive_reads.empty(), we don't queue new readers before
    // evicting all inactive reads.
    if (_wait_list.empty()) {
        const auto [it, _] = _inactive_reads.emplace(_next_id++, std::move(ir));
        (void)_;
        ++_inactive_read_stats.population;
        return inactive_read_handle(it->first);
    }

    // The evicted reader will release its permit, hopefully allowing us to
    // admit some readers from the _wait_list.
    ir->evict();
    ++_inactive_read_stats.permit_based_evictions;
    return inactive_read_handle();
}

std::unique_ptr<reader_concurrency_semaphore::inactive_read> reader_concurrency_semaphore::unregister_inactive_read(inactive_read_handle irh) {
    if (auto it = _inactive_reads.find(irh._id); it != _inactive_reads.end()) {
        auto ir = std::move(it->second);
        _inactive_reads.erase(it);
        --_inactive_read_stats.population;
        return ir;
    }
    return {};
}

bool reader_concurrency_semaphore::try_evict_one_inactive_read() {
    if (_inactive_reads.empty()) {
        return false;
    }
    auto it = _inactive_reads.begin();
    it->second->evict();
    _inactive_reads.erase(it);

    ++_inactive_read_stats.permit_based_evictions;
    --_inactive_read_stats.population;

    return true;
}

future<reader_permit> reader_concurrency_semaphore::wait_admission(size_t memory,
        db::timeout_clock::time_point timeout) {
    if (_wait_list.size() >= _max_queue_length) {
        if (_prethrow_action) {
            _prethrow_action();
        }
        return make_exception_future<reader_permit>(
                std::make_exception_ptr(std::runtime_error(
                        format("{}: restricted mutation reader queue overload", _name))));
    }
    auto r = resources(1, static_cast<ssize_t>(memory));
    auto it = _inactive_reads.begin();
    while (!may_proceed(r) && it != _inactive_reads.end()) {
        auto ir = std::move(it->second);
        it = _inactive_reads.erase(it);
        ir->evict();

        ++_inactive_read_stats.permit_based_evictions;
        --_inactive_read_stats.population;
    }
    if (may_proceed(r)) {
        _resources -= r;
        return make_ready_future<reader_permit>(reader_permit(*this, r));
    }
    promise<reader_permit> pr;
    auto fut = pr.get_future();
    _wait_list.push_back(entry(std::move(pr), r), timeout);
    return fut;
}

reader_permit reader_concurrency_semaphore::consume_resources(resources r) {
    _resources -= r;
    return reader_permit(*this, r);
}

// A file that tracks the memory usage of buffers resulting from read
// operations.
class tracking_file_impl : public file_impl {
    file _tracked_file;
    reader_permit _permit;

public:
    tracking_file_impl(file file, reader_permit permit)
        : _tracked_file(std::move(file))
        , _permit(std::move(permit)) {
    }

    tracking_file_impl(const tracking_file_impl&) = delete;
    tracking_file_impl& operator=(const tracking_file_impl&) = delete;
    tracking_file_impl(tracking_file_impl&&) = default;
    tracking_file_impl& operator=(tracking_file_impl&&) = default;

    virtual future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc) override {
        return get_file_impl(_tracked_file)->write_dma(pos, buffer, len, pc);
    }

    virtual future<size_t> write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) override {
        return get_file_impl(_tracked_file)->write_dma(pos, std::move(iov), pc);
    }

    virtual future<size_t> read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc) override {
        return get_file_impl(_tracked_file)->read_dma(pos, buffer, len, pc);
    }

    virtual future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) override {
        return get_file_impl(_tracked_file)->read_dma(pos, iov, pc);
    }

    virtual future<> flush(void) override {
        return get_file_impl(_tracked_file)->flush();
    }

    virtual future<struct stat> stat(void) override {
        return get_file_impl(_tracked_file)->stat();
    }

    virtual future<> truncate(uint64_t length) override {
        return get_file_impl(_tracked_file)->truncate(length);
    }

    virtual future<> discard(uint64_t offset, uint64_t length) override {
        return get_file_impl(_tracked_file)->discard(offset, length);
    }

    virtual future<> allocate(uint64_t position, uint64_t length) override {
        return get_file_impl(_tracked_file)->allocate(position, length);
    }

    virtual future<uint64_t> size(void) override {
        return get_file_impl(_tracked_file)->size();
    }

    virtual future<> close() override {
        return get_file_impl(_tracked_file)->close();
    }

    virtual std::unique_ptr<file_handle_impl> dup() override {
        return get_file_impl(_tracked_file)->dup();
    }

    virtual subscription<directory_entry> list_directory(std::function<future<> (directory_entry de)> next) override {
        return get_file_impl(_tracked_file)->list_directory(std::move(next));
    }

    virtual future<temporary_buffer<uint8_t>> dma_read_bulk(uint64_t offset, size_t range_size, const io_priority_class& pc) override {
        return get_file_impl(_tracked_file)->dma_read_bulk(offset, range_size, pc).then([this, units = _permit.get_memory_units(range_size)] (temporary_buffer<uint8_t> buf) {
            if (_permit) {
                buf = make_tracked_temporary_buffer(std::move(buf), _permit);
            }
            return make_ready_future<temporary_buffer<uint8_t>>(std::move(buf));
        });
    }
};

file make_tracked_file(file f, reader_permit p) {
    return file(make_shared<tracking_file_impl>(f, std::move(p)));
}

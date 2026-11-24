#pragma once

#include <cstdint>
#include <string>

class IStorage;

/**
 * Takes files from a web browser and writes them to the storage.
 *
 * One page, one request at a time, no threads: poll() is called from the user interface
 * and does a slice of work. Whoever opens the page needs the six digits shown on the
 * device before anything is written.
 */
class UploadServer
{
  public:
    static constexpr uint16_t c_port = 8080;

    explicit UploadServer(IStorage &storage);
    ~UploadServer();

    // work for at most budget milliseconds
    void poll(uint32_t budget);

    const char *code(void) const;
    // a file is arriving right now
    bool busy(void) const;
    // path of the file that is arriving or arrived last
    const std::string &file(void) const;
    uint64_t received(void) const;
    uint64_t expected(void) const;
    uint32_t saved(void) const;
    uint32_t failed(void) const;

  private:
    struct Impl;
    Impl *impl;
};

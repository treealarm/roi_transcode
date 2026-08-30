#pragma once

// How a backend reports what happened -- TZ v2.0 §7, where every method returns Status or
// Result<T> rather than throwing.
//
// The library's own entry points (transcode_file, Session) still throw: a caller that hands
// over a whole file wants one answer at the end, and turning every internal failure into a
// returned code on the way there would bury the reason. What §7 governs is the *backend*
// boundary, and there a returned status is right for a reason that has nothing to do with
// taste -- "this backend did not open" is the normal case on a machine without that GPU,
// not an error, and the caller's response is to try the next backend in the chain.

#include <stdexcept>
#include <string>
#include <utility>

namespace sve {

enum class StatusCode {
    Ok = 0,
    /// The backend is not present on this machine. Not a failure: the caller moves on.
    Unavailable,
    /// The caller asked for something this backend cannot do.
    Unsupported,
    /// The request itself does not make sense -- a plane shorter than its own stride
    /// claims, a format paired with a profile that cannot carry it. TZ v3.4 §25.
    InvalidArgument,
    /// Correct request, wrong moment -- no packet ready yet, more input needed.
    Again,
    /// No more output will come.
    EndOfStream,
    /// Everything else.
    Error,
};

class Status {
public:
    Status() = default;
    Status(StatusCode code, std::string message)
        : m_code(code), m_message(std::move(message))
    {
    }

    static Status Ok() { return {}; }
    static Status Unavailable(std::string m) { return {StatusCode::Unavailable, std::move(m)}; }
    static Status Unsupported(std::string m) { return {StatusCode::Unsupported, std::move(m)}; }
    static Status InvalidArgument(std::string m)
    {
        return {StatusCode::InvalidArgument, std::move(m)};
    }
    static Status Again() { return {StatusCode::Again, {}}; }
    static Status EndOfStream() { return {StatusCode::EndOfStream, {}}; }
    static Status Error(std::string m) { return {StatusCode::Error, std::move(m)}; }

    StatusCode code() const { return m_code; }
    const std::string& message() const { return m_message; }
    bool ok() const { return m_code == StatusCode::Ok; }
    explicit operator bool() const { return ok(); }

private:
    StatusCode m_code = StatusCode::Ok;
    std::string m_message;
};

/// A value, or the reason there is none. Deliberately not std::expected: that is C++23 and
/// this header is part of what an embedder compiles.
template <typename T>
class Result {
public:
    Result(T value) : m_value(std::move(value)) {}
    Result(Status status) : m_status(std::move(status))
    {
        // A Result carrying Ok but no value would read as success at every call site that
        // checks only the status, so the two cannot be built from one another by accident.
        if (m_status.ok()) {
            m_status = Status::Error("a Result was built from an Ok status without a value");
        }
    }

    bool ok() const { return m_status.ok(); }
    explicit operator bool() const { return ok(); }
    const Status& status() const { return m_status; }

    /// The value. Throws std::logic_error when there is none.
    ///
    /// It used to return a default-constructed T instead, which is the worst of the three
    /// options: a caller who forgot to check got a plausible-looking empty answer and
    /// carried on. A throw here is not error handling -- it is a bug being reported, and
    /// the message names the failure that was ignored.
    const T& value() const
    {
        require_value();
        return m_value;
    }
    T& value()
    {
        require_value();
        return m_value;
    }

private:
    void require_value() const
    {
        if (!m_status.ok()) {
            throw std::logic_error(
                "Result::value() on a failed result: " + m_status.message());
        }
    }

    Status m_status;
    T m_value{};
};

} // namespace sve

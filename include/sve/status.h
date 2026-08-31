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
    /// The device this backend needs is not on this machine. Not a failure: the caller
    /// moves to the next backend in the chain. Named DeviceUnavailable after TZ v3.4 §25.
    DeviceUnavailable,
    /// The caller asked for something this backend cannot do.
    Unsupported,
    /// The request itself does not make sense -- a plane shorter than its own stride
    /// claims, a format paired with a profile that cannot carry it. TZ v3.4 §25.
    InvalidArgument,
    /// Correct request, wrong moment -- no packet ready yet, more input needed.
    Again,
    /// No more output will come.
    EndOfStream,
    /// The backend failed at what it was asked to do. Carries the backend, the codec and
    /// the native code in its message (TZ v3.4 §25).
    BackendError,
    /// An allocation failed.
    OutOfMemory,
    /// A bounded wait expired. Only the optional blocking variants of §24 produce this;
    /// the normative API is non-blocking and answers Again instead.
    Timeout,
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
    static Status DeviceUnavailable(std::string m)
    {
        return {StatusCode::DeviceUnavailable, std::move(m)};
    }
    static Status Unsupported(std::string m) { return {StatusCode::Unsupported, std::move(m)}; }
    static Status InvalidArgument(std::string m)
    {
        return {StatusCode::InvalidArgument, std::move(m)};
    }
    static Status Again() { return {StatusCode::Again, {}}; }
    static Status EndOfStream() { return {StatusCode::EndOfStream, {}}; }
    static Status BackendError(std::string m)
    {
        return {StatusCode::BackendError, std::move(m)};
    }
    static Status OutOfMemory(std::string m)
    {
        return {StatusCode::OutOfMemory, std::move(m)};
    }
    static Status Timeout(std::string m) { return {StatusCode::Timeout, std::move(m)}; }
    static Status Error(std::string m) { return {StatusCode::Error, std::move(m)}; }

    StatusCode code() const { return m_code; }
    const std::string& message() const { return m_message; }
    bool ok() const { return m_code == StatusCode::Ok; }
    explicit operator bool() const { return ok(); }

private:
    StatusCode m_code = StatusCode::Ok;
    std::string m_message;
};

/// How the factory functions report a failure -- TZ v4.1 §8.12.
///
/// An exception rather than a returned Result, because §8.12 asks for one, and the reason
/// it can ask is that creating an encoder is not the case Status exists for. Status is
/// right at the backend boundary, where "this GPU is not here" is the ordinary answer and
/// the caller's move is to try the next backend. By the time a factory returns, that walk
/// is over: what is left is a request that cannot be served at all.
///
/// It carries the whole Status and not only its text, so a caller can branch on the code
/// without parsing a message -- InvalidArgument means fix the configuration, Unsupported
/// means this machine will never do it. Derived from std::runtime_error so that a caller
/// who catches std::exception, as both of this library's own entry points do, still does.
class EncoderError : public std::runtime_error {
public:
    explicit EncoderError(Status status)
        : std::runtime_error(status.message().empty() ? "encoder could not be created"
                                                      : status.message()),
          m_status(std::move(status))
    {
    }

    const Status& status() const noexcept { return m_status; }
    StatusCode code() const noexcept { return m_status.code(); }

private:
    Status m_status;
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

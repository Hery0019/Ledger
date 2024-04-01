#include "core/result.h"

namespace ledger {

std::string_view errorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::SyntaxError:         return "SyntaxError";
        case ErrorCode::TypeError:           return "TypeError";
        case ErrorCode::ConstraintViolation: return "ConstraintViolation";
        case ErrorCode::NotFound:            return "NotFound";
        case ErrorCode::AlreadyExists:       return "AlreadyExists";
        case ErrorCode::IoError:             return "IoError";
        case ErrorCode::Corruption:          return "Corruption";
        case ErrorCode::Internal:            return "Internal";
    }
    return "UnknownError";
}

}  // namespace ledger

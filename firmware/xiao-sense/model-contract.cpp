// Release tooling injects the exact verified model pack contract into the prepared Chakshu source.
// These sentinels must never reach a compiled Chakshu image.
namespace ChakshuModel {
constexpr size_t MODEL_BYTES=0;
constexpr char MODEL_SHA256[]="0000000000000000000000000000000000000000000000000000000000000000";
}

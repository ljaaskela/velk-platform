layout(set = 1, binding = 16, std430) buffer ProbeWords { uint data[]; } probe_words;
#define VELK_LIGHT_WORDS probe_words
#define VELK_PROBE_PIXELS 16u

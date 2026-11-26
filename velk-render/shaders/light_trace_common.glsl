// The point lists in the order they are traced.
uint velk_points_list(uint k)
{
    return k == 0u ? VELK_LIGHT_LIST_UNTRACED : k == 1u ? VELK_LIGHT_LIST_COARSE : VELK_LIGHT_LIST_FINE;
}

// The threads of a work group of the trace.
#define VELK_LIGHT_TRACE_THREADS 64u

// The most lanes a point spreads its rays over: one a ray, of a ray each of its lights at most.
#define VELK_LIGHT_RAY_LANES 8u

#include "range.h"
#include "common.h"

void Range::mergeBeginRange(modelclock_t begin) {
	if(begin > beginR) {
		beginR = begin;
	}
}
void Range::minMergeEndgeRange(modelclock_t end) {
	if(end < endR) {
		endR = end;
	}
}

bool Range::hastIntersection(Range &r) {
	return !(r.beginR > endR || r.endR < beginR);
}


void Range::print() const {
	model_print("%u => %u\n", beginR, endR);
}
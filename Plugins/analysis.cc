#include "analysis.h"
#include "model.h"
#include "action.h"
#include "execution.h"

bool computeErrorKey(char * buffer, ModelAction *wrt) {
	const char * position = wrt->get_position();
	uint index = 0;
	while(position != NULL && position[index] != '\0') {
		if(position[index++] == ':') {
			return true;
		}
	}
	// wrt has preset position such as memset, memcpy, and etc.
	sprintf(buffer, "%s%p", wrt->get_position(), wrt->get_location());
	return false;
}
char * duplicateString(char * str) {
	char *copy = (char*)model_malloc(strlen(str) + 1);
	strcpy(copy, str);
	return copy;
}

void Analysis::ERROR(ModelExecution *exec, ModelAction * wrt, ModelAction *read, const char * message) {
	if(wrt->get_position()) {
		char buffer[1024];
		char * position = computeErrorKey(buffer, wrt) ? (char*)wrt->get_position() : buffer;
		if(errorSet.get(position) == NULL) {
			ASSERT(read && read->get_position());
			model->get_execution()->add_warning("ERROR: %s: %s ====> write: Seq_number=%u \t Execution=%p \t Address=%p \t Location=%s\t"
																					">>>>>>> Read by: Address=%p \t Location=%s\n",getName(), message, wrt->get_seq_number(),
																					exec, wrt->get_location(), wrt->get_position(), read->get_location(), read->get_position());
			errorSet.add(duplicateString(position));
		}
	} else {
		ASSERT(0);
	}
	num_total_bugs++;
}

void Analysis::WARNING(ModelExecution *exec, ModelAction * wrt, ModelAction *read, const char * message) {
	if(wrt->get_position()) {
		char buffer[1024];
		char * position = computeErrorKey(buffer, wrt) ? (char*)wrt->get_position() : buffer;
		if(warningSet.get(position) == NULL) {
			ASSERT(read && read->get_position());
			model->get_execution()->add_warning("WARNING: %s: %s ====> write: Seq_number=%u \t Execution=%p \t Address=%p \t Location=%s\t"
																					">>>>>>> Read by: Address=%p \t Location=%s\n",getName(), message, wrt->get_seq_number(),
																					exec, wrt->get_location(), wrt->get_position(), read->get_location(), read->get_position());
			warningSet.add(duplicateString(position));
		}
	} else {
		ASSERT(0);
	}
	num_total_warnings++;
}

unsigned int hashErrorPosition(char *position) {
	unsigned int hash = 0;
	uint32_t index = 0;
	while(position != NULL && position[index] != '\0') {
		hash = (hash << 2) ^ (unsigned int)(position[index++]);
	}
	return hash;
}

bool equalErrorPosition(char *p1,char *p2) {
	if(p1 == NULL || p2 == NULL) {
		return false;
	}
	return strcmp(p1, p2) == 0;
}

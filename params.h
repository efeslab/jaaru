#ifndef __PARAMS_H__
#define __PARAMS_H__

/**
 * Model checker parameter structure. Holds run-time configuration options for
 * the model checker.
 */
struct model_params {
	modelclock_t firstCrash;
	bool nofork;
	uint storebufferthreshold;
	uint evictmax;
	int verbose;
	uint pmdebug;
	bool printSpace;
	uint numcrashes;
	char *file;
	bool enableCrash;
	bool failStop;
};

void param_defaults(struct model_params *params);

#endif	/* __PARAMS_H__ */

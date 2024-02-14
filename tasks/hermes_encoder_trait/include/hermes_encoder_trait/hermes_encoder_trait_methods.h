#ifndef HRUN_HERMES_ENCODER_TRAIT_METHODS_H_
#define HRUN_HERMES_ENCODER_TRAIT_METHODS_H_

/** The set of methods in the admin task */
struct Method : public TaskMethod {
  TASK_METHOD_T kEncode = kLast + 0;
  TASK_METHOD_T kDecode = kLast + 1;
};

#endif  // HRUN_HERMES_ENCODER_TRAIT_METHODS_H_

#include "java.h"
#include <string.h>
#include <unistd.h>
#include "javaObject.h"
#include "methodCallBaton.h"
#include "node_NodeDynamicProxyClass.h"
#include <sstream>

std::string nativeBindingLocation;
long v8ThreadId;

/*static*/ v8::Persistent<v8::FunctionTemplate> Java::s_ct;

void my_sleep(int dur) {
#ifdef WIN32
  Sleep(dur);
#else
  usleep(dur);
#endif
}

long my_getThreadId() {
#ifdef WIN32
  return (long)GetCurrentThreadId();
#else
  return (long)pthread_self();
#endif
}

/*static*/ void Java::Init(v8::Handle<v8::Object> target) {
  v8::HandleScope scope;

  v8ThreadId = my_getThreadId();

  v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(New);
  s_ct = v8::Persistent<v8::FunctionTemplate>::New(t);
  s_ct->InstanceTemplate()->SetInternalFieldCount(1);
  s_ct->SetClassName(v8::String::NewSymbol("Java"));

  NODE_SET_PROTOTYPE_METHOD(s_ct, "newInstance", newInstance);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "newInstanceSync", newInstanceSync);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "newProxy", newProxy);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "callStaticMethod", callStaticMethod);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "callStaticMethodSync", callStaticMethodSync);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "findClassSync", findClassSync);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "newArray", newArray);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "newByte", newByte);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "getStaticFieldValue", getStaticFieldValue);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "setStaticFieldValue", setStaticFieldValue);

  target->Set(v8::String::NewSymbol("Java"), s_ct->GetFunction());
}

/*static*/ v8::Handle<v8::Value> Java::New(const v8::Arguments& args) {
  v8::HandleScope scope;

  Java *self = new Java();
  self->Wrap(args.This());

  self->handle_->Set(v8::String::New("classpath"), v8::Array::New());
  self->handle_->Set(v8::String::New("options"), v8::Array::New());
  self->handle_->Set(v8::String::New("nativeBindingLocation"), v8::String::New("Not Set"));

  return args.This();
}

Java::Java() {
  this->m_jvm = NULL;
  this->m_env = NULL;
}

Java::~Java() {

}

v8::Handle<v8::Value> Java::ensureJvm() {
  if(!m_jvm) {
    return createJVM(&this->m_jvm, &this->m_env);
  }

  return v8::Undefined();
}

v8::Handle<v8::Value> Java::createJVM(JavaVM** jvm, JNIEnv** env) {
  JavaVM* jvmTemp;
  JavaVMInitArgs args;

  // setup classpath
  std::ostringstream classPath;
  classPath << "-Djava.class.path=";

  v8::Local<v8::Value> classPathValue = handle_->Get(v8::String::New("classpath"));
  if(!classPathValue->IsArray()) {
    return ThrowException(v8::Exception::TypeError(v8::String::New("Classpath must be an array")));
  }
  v8::Local<v8::Array> classPathArray = v8::Array::Cast(*classPathValue);
  for(uint32_t i=0; i<classPathArray->Length(); i++) {
    if(i != 0) {
      #ifdef WIN32
        classPath << ";";
      #else
        classPath << ":";
      #endif
    }
    v8::Local<v8::Value> arrayItemValue = classPathArray->Get(i);
    if(!arrayItemValue->IsString()) {
      return ThrowException(v8::Exception::TypeError(v8::String::New("Classpath must only contain strings")));
    }
    v8::Local<v8::String> arrayItem = arrayItemValue->ToString();
    v8::String::AsciiValue arrayItemStr(arrayItem);
    classPath << *arrayItemStr;
  }

  // set the native binding location
  v8::Local<v8::Value> v8NativeBindingLocation = handle_->Get(v8::String::New("nativeBindingLocation"));
  v8::String::AsciiValue nativeBindingLocationStr(v8NativeBindingLocation);
  nativeBindingLocation = *nativeBindingLocationStr;

  // get other options
  v8::Local<v8::Value> optionsValue = handle_->Get(v8::String::New("options"));
  if(!optionsValue->IsArray()) {
    return ThrowException(v8::Exception::TypeError(v8::String::New("options must be an array")));
  }
  v8::Local<v8::Array> optionsArray = v8::Array::Cast(*optionsValue);

  // create vm options
  int vmOptionsCount = optionsArray->Length() + 1;
  JavaVMOption* vmOptions = new JavaVMOption[vmOptionsCount];
  vmOptions[0].optionString = strdup(classPath.str().c_str());
  for(uint32_t i=0; i<optionsArray->Length(); i++) {
    v8::Local<v8::Value> arrayItemValue = optionsArray->Get(i);
    if(!arrayItemValue->IsString()) {
      return ThrowException(v8::Exception::TypeError(v8::String::New("options must only contain strings")));
    }
    v8::Local<v8::String> arrayItem = arrayItemValue->ToString();
    v8::String::AsciiValue arrayItemStr(arrayItem);
    vmOptions[i+1].optionString = strdup(*arrayItemStr);
  }

  JNI_GetDefaultJavaVMInitArgs(&args);
  args.version = JNI_VERSION_1_6;
  args.ignoreUnrecognized = false;
  args.options = vmOptions;
  args.nOptions = vmOptionsCount;
  JNI_CreateJavaVM(&jvmTemp, (void **)env, &args);
  *jvm = jvmTemp;

  return v8::Undefined();
}

/*static*/ v8::Handle<v8::Value> Java::newInstance(const v8::Arguments& args) {
  v8::HandleScope scope;
  Java* self = node::ObjectWrap::Unwrap<Java>(args.This());
  v8::Handle<v8::Value> ensureJvmResults = self->ensureJvm();
  if(!ensureJvmResults->IsUndefined()) {
    return ensureJvmResults;
  }
  JNIEnv* env = self->getJavaEnv();

  int argsStart = 0;
  int argsEnd = args.Length();

  // arguments
  ARGS_FRONT_CLASSNAME();
  ARGS_BACK_CALLBACK();

  // find class
  jclass clazz = javaFindClass(env, className);
  if(clazz == NULL) {
    EXCEPTION_CALL_CALLBACK("Could not find class " << className.c_str());
    return v8::Undefined();
  }

  // get method
  jobjectArray methodArgs = v8ToJava(env, args, argsStart, argsEnd);
  jobject method = javaFindConstructor(env, clazz, methodArgs);
  if(method == NULL) {
    EXCEPTION_CALL_CALLBACK("Could not find constructor for class " << className);
    return v8::Undefined();
  }

  // run
  NewInstanceBaton* baton = new NewInstanceBaton(self, clazz, method, methodArgs, callback);
  baton->run();

  END_CALLBACK_FUNCTION("\"Constructor for class '" << className << "' called without a callback did you mean to use the Sync version?\"");
}

/*static*/ v8::Handle<v8::Value> Java::newInstanceSync(const v8::Arguments& args) {
  v8::HandleScope scope;
  Java* self = node::ObjectWrap::Unwrap<Java>(args.This());
  v8::Handle<v8::Value> ensureJvmResults = self->ensureJvm();
  if(!ensureJvmResults->IsUndefined()) {
    return ensureJvmResults;
  }
  JNIEnv* env = self->getJavaEnv();

  int argsStart = 0;
  int argsEnd = args.Length();

  // arguments
  ARGS_FRONT_CLASSNAME();

  // find class
  jclass clazz = javaFindClass(env, className);
  if(clazz == NULL) {
    std::ostringstream errStr;
    errStr << "Could not create class " << className.c_str();
    return ThrowException(javaExceptionToV8(env, errStr.str()));
  }

  // find method
  jobjectArray methodArgs = v8ToJava(env, args, argsStart, argsEnd);
  jobject method = javaFindConstructor(env, clazz, methodArgs);
  if(method == NULL) {
    std::ostringstream errStr;
    errStr << "Could not find constructor for class " << className.c_str();
    return ThrowException(javaExceptionToV8(env, errStr.str()));
  }

  // run
  v8::Handle<v8::Value> callback = v8::Object::New();
  NewInstanceBaton* baton = new NewInstanceBaton(self, clazz, method, methodArgs, callback);
  v8::Handle<v8::Value> result = baton->runSync();
  delete baton;
  if(result->IsNativeError()) {
    return ThrowException(result);
  }
  return scope.Close(result);
}

/*static*/ v8::Handle<v8::Value> Java::newProxy(const v8::Arguments& args) {
  v8::HandleScope scope;
  Java* self = node::ObjectWrap::Unwrap<Java>(args.This());
  v8::Handle<v8::Value> ensureJvmResults = self->ensureJvm();
  if(!ensureJvmResults->IsUndefined()) {
    return ensureJvmResults;
  }
  JNIEnv* env = self->getJavaEnv();

  int argsStart = 0;
  int argsEnd = args.Length();

  ARGS_FRONT_STRING(interfaceName);
  ARGS_FRONT_OBJECT(functions);

  DynamicProxyData* dynamicProxyData = new DynamicProxyData();
  dynamicProxyData->markerStart = DYNAMIC_PROXY_DATA_MARKER_START;
  dynamicProxyData->markerEnd = DYNAMIC_PROXY_DATA_MARKER_END;
  dynamicProxyData->java = self;
  dynamicProxyData->interfaceName = interfaceName;
  dynamicProxyData->functions = v8::Persistent<v8::Object>::New(functions);

  // find NodeDynamicProxyClass
  std::string className = "node.NodeDynamicProxyClass";
  jclass clazz = javaFindClass(env, className);
  if(clazz == NULL) {
    std::ostringstream errStr;
    errStr << "Could not create class node/NodeDynamicProxyClass";
    return ThrowException(javaExceptionToV8(env, errStr.str()));
  }

  // find constructor
  jclass objectClazz = env->FindClass("java/lang/Object");
  jobjectArray methodArgs = env->NewObjectArray(2, objectClazz, NULL);
  env->SetObjectArrayElement(methodArgs, 0, v8ToJava(env, v8::String::New(nativeBindingLocation.c_str())));
  env->SetObjectArrayElement(methodArgs, 1, longToJavaLongObj(env, (long)dynamicProxyData));
  jobject method = javaFindConstructor(env, clazz, methodArgs);
  if(method == NULL) {
    std::ostringstream errStr;
    errStr << "Could not find constructor for class node/NodeDynamicProxyClass";
    return ThrowException(javaExceptionToV8(env, errStr.str()));
  }

  // run constructor
  v8::Handle<v8::Value> callback = v8::Object::New();
  NewInstanceBaton* baton = new NewInstanceBaton(self, clazz, method, methodArgs, callback);
  v8::Handle<v8::Value> result = baton->runSync();
  delete baton;
  if(result->IsNativeError()) {
    return ThrowException(result);
  }
  return scope.Close(result);
}

/*static*/ v8::Handle<v8::Value> Java::callStaticMethod(const v8::Arguments& args) {
  v8::HandleScope scope;
  Java* self = node::ObjectWrap::Unwrap<Java>(args.This());
  v8::Handle<v8::Value> ensureJvmResults = self->ensureJvm();
  if(!ensureJvmResults->IsUndefined()) {
    return ensureJvmResults;
  }
  JNIEnv* env = self->getJavaEnv();

  int argsStart = 0;
  int argsEnd = args.Length();

  // arguments
  ARGS_FRONT_CLASSNAME();
  ARGS_FRONT_STRING(methodName);
  ARGS_BACK_CALLBACK();

  // find class
  jclass clazz = javaFindClass(env, className);
  if(clazz == NULL) {
    EXCEPTION_CALL_CALLBACK("Could not create class " << className.c_str());
    return v8::Undefined();
  }

  // find method
  jobjectArray methodArgs = v8ToJava(env, args, argsStart, argsEnd);
  jobject method = javaFindMethod(env, clazz, methodName, methodArgs);
  if(method == NULL) {
    EXCEPTION_CALL_CALLBACK("Could not find method \"" << methodName.c_str() << "\"");
    return v8::Undefined();
  }

  // run
  StaticMethodCallBaton* baton = new StaticMethodCallBaton(self, clazz, method, methodArgs, callback);
  baton->run();

  END_CALLBACK_FUNCTION("\"Static method '" << methodName << "' called without a callback did you mean to use the Sync version?\"");
}

/*static*/ v8::Handle<v8::Value> Java::callStaticMethodSync(const v8::Arguments& args) {
  v8::HandleScope scope;
  Java* self = node::ObjectWrap::Unwrap<Java>(args.This());
  v8::Handle<v8::Value> ensureJvmResults = self->ensureJvm();
  if(!ensureJvmResults->IsUndefined()) {
    return ensureJvmResults;
  }
  JNIEnv* env = self->getJavaEnv();

  int argsStart = 0;
  int argsEnd = args.Length();

  // arguments
  ARGS_FRONT_CLASSNAME();
  ARGS_FRONT_STRING(methodName);

  // find class
  jclass clazz = javaFindClass(env, className);
  if(clazz == NULL) {
    std::ostringstream errStr;
    errStr << "Could not create class " << className.c_str();
    return ThrowException(javaExceptionToV8(env, errStr.str()));
  }

  // find method
  jobjectArray methodArgs = v8ToJava(env, args, argsStart, argsEnd);
  jobject method = javaFindMethod(env, clazz, methodName, methodArgs);
  if(method == NULL) {
    std::ostringstream errStr;
    errStr << "Could not find method \"" << methodName.c_str() << "\"";
    return ThrowException(javaExceptionToV8(env, errStr.str()));
  }

  // run
  v8::Handle<v8::Value> callback = v8::Object::New();
  StaticMethodCallBaton* baton = new StaticMethodCallBaton(self, clazz, method, methodArgs, callback);
  v8::Handle<v8::Value> result = baton->runSync();
  delete baton;
  if(result->IsNativeError()) {
    return ThrowException(result);
  }
  return scope.Close(result);
}

/*static*/ v8::Handle<v8::Value> Java::findClassSync(const v8::Arguments& args) {
  v8::HandleScope scope;
  Java* self = node::ObjectWrap::Unwrap<Java>(args.This());
  v8::Handle<v8::Value> ensureJvmResults = self->ensureJvm();
  if(!ensureJvmResults->IsUndefined()) {
    return ensureJvmResults;
  }
  JNIEnv* env = self->getJavaEnv();

  int argsStart = 0;
  int argsEnd = args.Length();

  // arguments
  ARGS_FRONT_CLASSNAME();

  // find class
  jclass clazz = javaFindClass(env, className);
  if(clazz == NULL) {
    std::ostringstream errStr;
    errStr << "Could not create class " << className.c_str();
    return ThrowException(javaExceptionToV8(env, errStr.str()));
  }

  // run
  v8::Handle<v8::Value> result = javaToV8(self, env, clazz);
  return scope.Close(result);
}

/*static*/ v8::Handle<v8::Value> Java::newArray(const v8::Arguments& args) {
  v8::HandleScope scope;
  Java* self = node::ObjectWrap::Unwrap<Java>(args.This());
  v8::Handle<v8::Value> ensureJvmResults = self->ensureJvm();
  if(!ensureJvmResults->IsUndefined()) {
    return ensureJvmResults;
  }
  JNIEnv* env = self->getJavaEnv();

  int argsStart = 0;
  int argsEnd = args.Length();

  // arguments
  ARGS_FRONT_CLASSNAME();

  // argument - array
  if(args.Length() < argsStart+1 || !args[argsStart]->IsArray()) {
    std::ostringstream errStr;
    errStr << "Argument " << (argsStart+1) << " must be an array";
    return ThrowException(v8::Exception::TypeError(v8::String::New(errStr.str().c_str())));
  }
  v8::Local<v8::Array> arrayObj = v8::Local<v8::Array>::Cast(args[argsStart]);

  UNUSED_VARIABLE(argsEnd);

  // find class and method
  jarray results;
  if(strcmp(className.c_str(), "byte") == 0) {
    results = env->NewByteArray(arrayObj->Length());
    for(uint32_t i=0; i<arrayObj->Length(); i++) {
      v8::Local<v8::Value> item = arrayObj->Get(i);
      jobject val = v8ToJava(env, item);
      jclass byteClazz = env->FindClass("java/lang/Byte");
      jmethodID byte_byteValue = env->GetMethodID(byteClazz, "byteValue", "()B");
      jbyte byteValues[1];
      byteValues[0] = env->CallByteMethod(val, byte_byteValue);
      env->SetByteArrayRegion((jbyteArray)results, i, 1, byteValues);
    }
  }

  else
  {
    jclass clazz = javaFindClass(env, className);
    if(clazz == NULL) {
      std::ostringstream errStr;
      errStr << "Could not create class " << className.c_str();
      return ThrowException(javaExceptionToV8(env, errStr.str()));
    }

    // create array
    results = env->NewObjectArray(arrayObj->Length(), clazz, NULL);

    for(uint32_t i=0; i<arrayObj->Length(); i++) {
      v8::Local<v8::Value> item = arrayObj->Get(i);
      jobject val = v8ToJava(env, item);
      env->SetObjectArrayElement((jobjectArray)results, i, val);
      if(env->ExceptionOccurred()) {
        std::ostringstream errStr;
        v8::String::AsciiValue valStr(item);
        errStr << "Could not add item \"" << *valStr << "\" to array.";
        return ThrowException(javaExceptionToV8(env, errStr.str()));
      }
    }
  }

  return scope.Close(JavaObject::New(self, results));
}

/*static*/ v8::Handle<v8::Value> Java::newByte(const v8::Arguments& args) {
  v8::HandleScope scope;
  Java* self = node::ObjectWrap::Unwrap<Java>(args.This());
  v8::Handle<v8::Value> ensureJvmResults = self->ensureJvm();
  if(!ensureJvmResults->IsUndefined()) {
    return ensureJvmResults;
  }
  JNIEnv* env = self->getJavaEnv();

  if(args.Length() != 1) {
    return ThrowException(v8::Exception::TypeError(v8::String::New("newByte only takes 1 argument")));
  }

  // argument - value
  if(!args[0]->IsNumber()) {
    return ThrowException(v8::Exception::TypeError(v8::String::New("Argument 1 must be a number")));
  }

  v8::Local<v8::Number> val = args[0]->ToNumber();

  jclass clazz = env->FindClass("java/lang/Byte");
  jmethodID constructor = env->GetMethodID(clazz, "<init>", "(B)V");
  jobject newObj = env->NewObject(clazz, constructor, (jbyte)val->Value());

  return scope.Close(JavaObject::New(self, newObj));
}

/*static*/ v8::Handle<v8::Value> Java::getStaticFieldValue(const v8::Arguments& args) {
  v8::HandleScope scope;
  Java* self = node::ObjectWrap::Unwrap<Java>(args.This());
  v8::Handle<v8::Value> ensureJvmResults = self->ensureJvm();
  if(!ensureJvmResults->IsUndefined()) {
    return ensureJvmResults;
  }
  JNIEnv* env = self->getJavaEnv();
  PUSH_LOCAL_JAVA_FRAME();

  int argsStart = 0;
  int argsEnd = args.Length();

  // arguments
  ARGS_FRONT_CLASSNAME();
  ARGS_FRONT_STRING(fieldName);
  UNUSED_VARIABLE(argsEnd);

  // find the class
  jclass clazz = javaFindClass(env, className);
  if(clazz == NULL) {
    std::ostringstream errStr;
    errStr << "Could not create class " << className.c_str();
    POP_LOCAL_JAVA_FRAME_AND_RETURN(ThrowException(javaExceptionToV8(env, errStr.str())));
  }

  // get the field
  jobject field = javaFindField(env, clazz, fieldName);
  if(field == NULL) {
    std::ostringstream errStr;
    errStr << "Could not find field " << fieldName.c_str() << " on class " << className.c_str();
    POP_LOCAL_JAVA_FRAME_AND_RETURN(ThrowException(javaExceptionToV8(env, errStr.str())));
  }

  jclass fieldClazz = env->FindClass("java/lang/reflect/Field");
  jmethodID field_get = env->GetMethodID(fieldClazz, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");

  // get field value
  jobject val = env->CallObjectMethod(field, field_get, NULL);
  if(env->ExceptionOccurred()) {
    std::ostringstream errStr;
    errStr << "Could not get field " << fieldName.c_str() << " on class " << className.c_str();
    POP_LOCAL_JAVA_FRAME_AND_RETURN(ThrowException(javaExceptionToV8(env, errStr.str())));
  }

  env->DeleteLocalRef(clazz);
  env->DeleteLocalRef(field);
  env->DeleteLocalRef(fieldClazz);

  POP_LOCAL_JAVA_FRAME_AND_RETURN(scope.Close(javaToV8(self, env, val)));
}

/*static*/ v8::Handle<v8::Value> Java::setStaticFieldValue(const v8::Arguments& args) {
  v8::HandleScope scope;
  Java* self = node::ObjectWrap::Unwrap<Java>(args.This());
  v8::Handle<v8::Value> ensureJvmResults = self->ensureJvm();
  if(!ensureJvmResults->IsUndefined()) {
    return ensureJvmResults;
  }
  JNIEnv* env = self->getJavaEnv();
  PUSH_LOCAL_JAVA_FRAME();

  int argsStart = 0;
  int argsEnd = args.Length();

  // arguments
  ARGS_FRONT_CLASSNAME();
  ARGS_FRONT_STRING(fieldName);

  // argument - new value
  if(args.Length() < argsStart+1) {
    std::ostringstream errStr;
    errStr << "setStaticFieldValue requires " << (argsStart+1) << " arguments";
    POP_LOCAL_JAVA_FRAME_AND_RETURN(ThrowException(v8::Exception::TypeError(v8::String::New(errStr.str().c_str()))));
  }
  jobject newValue = v8ToJava(env, args[argsStart]);
  argsStart++;

  UNUSED_VARIABLE(argsEnd);

  // find the class
  jclass clazz = javaFindClass(env, className);
  if(clazz == NULL) {
    std::ostringstream errStr;
    errStr << "Could not create class " << className.c_str();
    POP_LOCAL_JAVA_FRAME_AND_RETURN(ThrowException(javaExceptionToV8(env, errStr.str())));
  }

  // get the field
  jobject field = javaFindField(env, clazz, fieldName);
  if(field == NULL) {
    std::ostringstream errStr;
    errStr << "Could not find field " << fieldName.c_str() << " on class " << className.c_str();
    POP_LOCAL_JAVA_FRAME_AND_RETURN(ThrowException(javaExceptionToV8(env, errStr.str())));
  }

  jclass fieldClazz = env->FindClass("java/lang/reflect/Field");
  jmethodID field_set = env->GetMethodID(fieldClazz, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V");

  //printf("newValue: %s\n", javaObjectToString(env, newValue).c_str());

  // set field value
  env->CallObjectMethod(field, field_set, NULL, newValue);
  if(env->ExceptionOccurred()) {
    std::ostringstream errStr;
    errStr << "Could not set field " << fieldName.c_str() << " on class " << className.c_str();
    POP_LOCAL_JAVA_FRAME_AND_RETURN(ThrowException(javaExceptionToV8(env, errStr.str())));
  }

  POP_LOCAL_JAVA_FRAME_AND_RETURN(v8::Undefined());
}

void EIO_CallJs(uv_work_t* req) {
}

void EIO_AfterCallJs(uv_work_t* req) {
  DynamicProxyData* dynamicProxyData = static_cast<DynamicProxyData*>(req->data);
  if(!dynamicProxyDataVerify(dynamicProxyData)) {
    return;
  }
  dynamicProxyData->result = NULL;

  JNIEnv* env = dynamicProxyData->env;

  v8::HandleScope scope;
  v8::Array* v8Args;
  v8::Function* fn;
  v8::Handle<v8::Value>* argv;
  int argc;
  int i;
  v8::Local<v8::Value> v8Result;
  jobject javaResult;

  v8::Local<v8::Value> fnObj = dynamicProxyData->functions->Get(v8::String::New(dynamicProxyData->methodName.c_str()));
  if(fnObj->IsUndefined() || fnObj->IsNull()) {
    printf("ERROR: Could not find method %s\n", dynamicProxyData->methodName.c_str());
    goto CleanUp;
  }
  if(!fnObj->IsFunction()) {
    printf("ERROR: %s is not a function.\n", dynamicProxyData->methodName.c_str());
    goto CleanUp;
  }

  fn = v8::Function::Cast(*fnObj);

  if(dynamicProxyData->args) {
    v8Args = v8::Array::Cast(*javaArrayToV8(dynamicProxyData->java, env, dynamicProxyData->args));
    argc = v8Args->Length();
  } else {
    argc = 0;
  }
  argv = new v8::Handle<v8::Value>[argc];
  for(i=0; i<argc; i++) {
    argv[i] = v8Args->Get(i);
  }
  v8Result = fn->Call(dynamicProxyData->functions, argc, argv);
  delete[] argv;
  if(!dynamicProxyDataVerify(dynamicProxyData)) {
    return;
  }

  javaResult = v8ToJava(env, v8Result);
  if(javaResult == NULL) {
    dynamicProxyData->result = NULL;
  } else {
    dynamicProxyData->result = env->NewGlobalRef(javaResult);
  }

CleanUp:
  dynamicProxyData->done = true;
}

JNIEXPORT jobject JNICALL Java_node_NodeDynamicProxyClass_callJs(JNIEnv *env, jobject src, jlong ptr, jobject method, jobjectArray args) {
  long myThreadId = my_getThreadId();

  DynamicProxyData* dynamicProxyData = (DynamicProxyData*)ptr;
  dynamicProxyData->env = env;
  dynamicProxyData->args = args;
  dynamicProxyData->done = false;
  dynamicProxyData->result = NULL;

  jclass methodClazz = env->FindClass("java/lang/reflect/Method");
  jmethodID method_getName = env->GetMethodID(methodClazz, "getName", "()Ljava/lang/String;");
  dynamicProxyData->methodName = javaObjectToString(env, env->CallObjectMethod(method, method_getName));

  uv_work_t* req = new uv_work_t();
  req->data = dynamicProxyData;
  if(myThreadId == v8ThreadId) {
    EIO_AfterCallJs(req);
  } else {
    uv_queue_work(uv_default_loop(), req, EIO_CallJs, EIO_AfterCallJs);

    while(!dynamicProxyData->done) {
      my_sleep(100);
    }
  }

  if(!dynamicProxyDataVerify(dynamicProxyData)) {
    return NULL;
  }
  if(dynamicProxyData->result) {
    env->DeleteGlobalRef(dynamicProxyData->result);
  }
  return dynamicProxyData->result;
}

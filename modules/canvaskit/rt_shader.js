CanvasKit._extraInitializations = CanvasKit._extraInitializations || [];
CanvasKit._extraInitializations.push(function() {

  // Compiles sksl with one of the _Make* bindings. errorCallback gets the error string if the
  // effect cannot be made; without one, the error is logged.
  function makeEffect(make, sksl, errorCallback) {
    // The easiest way to pass a function into C++ code is to wrap it in an object and
    // treat it as an emscripten::val on the other side.
    var callbackObj = {
      'onError': errorCallback || function(err) {
        console.log('RuntimeEffect error', err);
      },
    };
    return make(sksl, callbackObj);
  }

  // Copies the children's bare pointers (re-wrapped as sk_sp in C++); the caller frees the result.
  function copyChildPointers(children) {
    var barePointers = [];
    for (var i = 0; i < children.length; i++) {
      barePointers.push(children[i].$$.ptr);
    }
    return copy1dArray(barePointers, 'HEAPU32');
  }

  CanvasKit.RuntimeEffect.prototype.makeBuilder = function(uniforms) {
    var ptr = copy1dArray(uniforms, 'HEAPF32');
    try { return this._makeBuilder(ptr, uniforms.length * 4); }
    finally { freeArraysThatAreNotMallocedByUsers(ptr, uniforms); }
  };

  CanvasKit.RuntimeEffectBuilder.prototype.setUniforms = function(uniforms) {
    var ptr = copy1dArray(uniforms, 'HEAPF32');
    try { return this._setUniforms(ptr, uniforms.length * 4); }
    finally { freeArraysThatAreNotMallocedByUsers(ptr, uniforms); }
  };

  CanvasKit.ImageFilter.MakeRuntimeShader = function(builder, childNames, inputs, sampleRadius) {
    return CanvasKit.ImageFilter._MakeRuntimeShader(builder, childNames, inputs,
                                                    sampleRadius === undefined ? 0 : sampleRadius);
  };

  // sksl is the shader code.
  CanvasKit.RuntimeEffect.Make = function(sksl, errorCallback) {
    return makeEffect(CanvasKit.RuntimeEffect._Make, sksl, errorCallback);
  };

  // sksl is the blender code.
  CanvasKit.RuntimeEffect.MakeForBlender = function(sksl, errorCallback) {
    return makeEffect(CanvasKit.RuntimeEffect._MakeForBlender, sksl, errorCallback);
  };

  // sksl is the color filter code: half4 main(half4 color).
  CanvasKit.RuntimeEffect.MakeForColorFilter = function(sksl, errorCallback) {
    return makeEffect(CanvasKit.RuntimeEffect._MakeForColorFilter, sksl, errorCallback);
  };

  CanvasKit.RuntimeEffect.prototype.makeShader = function(floats, localMatrix) {
    // If the uniforms were set in a MallocObj, we don't want the shader to take ownership of
    // them (and free the memory when the shader is freed).
    var shouldOwnUniforms = !floats['_ck'];
    var fptr = copy1dArray(floats, 'HEAPF32');
    var localMatrixPtr = copy3x3MatrixToWasm(localMatrix);
    // Our array has 4 bytes per float, so be sure to account for that before
    // sending it over the wire.
    return this._makeShader(fptr, floats.length * 4, shouldOwnUniforms, localMatrixPtr);
  }

  // childrenWithShaders is an array of other shaders (e.g. Image.makeShader())
  CanvasKit.RuntimeEffect.prototype.makeShaderWithChildren = function(floats, childrenShaders, localMatrix) {
    // If the uniforms were set in a MallocObj, we don't want the shader to take ownership of
    // them (and free the memory when the shader is freed).
    var shouldOwnUniforms = !floats['_ck'];
    var fptr = copy1dArray(floats, 'HEAPF32');
    var localMatrixPtr = copy3x3MatrixToWasm(localMatrix);
    var children = childrenShaders || [];
    var childrenPointers = copyChildPointers(children);
    // Our array has 4 bytes per float, so be sure to account for that before
    // sending it over the wire.
    var shader = this._makeShaderWithChildren(fptr, floats.length * 4, shouldOwnUniforms,
                                              childrenPointers, children.length, localMatrixPtr);
    CanvasKit._free(childrenPointers);
    return shader;
  }

  // childrenShaders (optional) are the effect's `uniform shader` children, in declaration order.
  CanvasKit.RuntimeEffect.prototype.makeColorFilter = function(floats, childrenShaders) {
    // A MallocObj keeps its memory; plain arrays are copied and owned by the color filter.
    var shouldOwnUniforms = !floats['_ck'];
    var fptr = copy1dArray(floats, 'HEAPF32');
    var children = childrenShaders || [];
    var childrenPointers = copyChildPointers(children);
    var filter = this._makeColorFilter(fptr, floats.length * 4, shouldOwnUniforms,
                                       childrenPointers, children.length);
    CanvasKit._free(childrenPointers);
    return filter;
  }

  CanvasKit.RuntimeEffect.prototype.makeBlender = function(floats) {
    // If the uniforms were set in a MallocObj, we don't want the shader to take ownership of
    // them (and free the memory when the blender is freed).
    var shouldOwnUniforms = !floats['_ck'];
    var fptr = copy1dArray(floats, 'HEAPF32');
    return this._makeBlender(fptr, floats.length * 4, shouldOwnUniforms);
  }
});

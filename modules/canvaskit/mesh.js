(function(CanvasKit) {
  CanvasKit._extraInitializations.push(function() {
    var attributeTypes = {'float':0, 'float2':1, 'float3':2, 'float4':3, 'ubyte4':4};
    var varyingTypes = {'float':0, 'float2':1, 'float3':2, 'float4':3,
                        'half':4, 'half2':5, 'half3':6, 'half4':7};
    function integer(n) { return Number.isSafeInteger(n) && n >= 0 && n <= 0xffffffff; }
    function descriptors(items, types, attribute) {
      if (!Array.isArray(items)) return null;
      var result = [];
      for (var i = 0; i < items.length; i++) {
        var item = items[i];
        if (!item || typeof item['name'] !== 'string' || !Object.prototype.hasOwnProperty.call(types, item['type']) ||
            (attribute && (!integer(item['offset']) || (item['rate'] !== undefined &&
             item['rate'] !== 'vertex' && item['rate'] !== 'instance')))) return null;
        result.push({'name':item['name'], 'type':types[item['type']], 'offset':item['offset'] || 0,
                     'rate':item['rate'] === 'instance' ? 1 : 0});
      }
      return result;
    }
    CanvasKit.MeshSpecification.Make = function(attributes, stride, varyings, vs, fs, cs, alpha, instanceStride) {
      instanceStride = instanceStride === undefined ? 0 : instanceStride;
      var a = descriptors(attributes, attributeTypes, true), v = descriptors(varyings, varyingTypes, false);
      if (!a || !v || !integer(stride) || !integer(instanceStride) || typeof vs !== 'string' || typeof fs !== 'string')
        return {'error':'Invalid mesh specification arguments'};
      return this._Make(a, stride, v, vs, fs, cs || CanvasKit.ColorSpace.SRGB,
                        alpha === undefined ? CanvasKit.AlphaType.Premul : alpha, instanceStride);
    };
    function bytes(data, index) {
      if (index ? !(data instanceof Uint16Array) :
          !(data instanceof Float32Array || data instanceof Uint8Array)) return null;
      return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    }
    function upload(context, data, index) {
      var array = bytes(data, index);
      if (!array || !array.byteLength) return null;
      if (context) CanvasKit.setCurrentContext(context._context);
      var ptr = CanvasKit._malloc(array.byteLength);
      try {
        CanvasKit.HEAPU8.set(array, ptr);
        var buffer = index ? CanvasKit._MakeMeshIndexBuffer(context, ptr, array.byteLength) :
                             CanvasKit._MakeMeshVertexBuffer(context, ptr, array.byteLength);
        if (buffer) buffer._meshContext = context;
        return buffer;
      } finally { CanvasKit._free(ptr); }
    }
    function update(offset, data, index) {
      var array = bytes(data, index);
      if (!array || !integer(offset) || offset % 4 || !array.byteLength || array.byteLength % 4 ||
          offset + array.byteLength > this['size']()) return false;
      if (this._meshContext) CanvasKit.setCurrentContext(this._meshContext._context);
      var ptr = CanvasKit._malloc(array.byteLength);
      try {
        CanvasKit.HEAPU8.set(array, ptr);
        return this._update(ptr, offset, array.byteLength);
      } finally { CanvasKit._free(ptr); }
    }
    CanvasKit.MakeMeshVertexBuffer = function(context, data) { return upload(context, data, false); };
    CanvasKit.MakeMeshIndexBuffer = function(context, data) { return upload(context, data, true); };
    CanvasKit.MeshVertexBuffer.prototype.update = function(offset, data) { return update.call(this, offset, data, false); };
    CanvasKit.MeshIndexBuffer.prototype.update = function(offset, data) { return update.call(this, offset, data, true); };
    CanvasKit.MakeMesh = function(options) {
      if (!options || typeof options !== 'object') return null;
      var spec = options['spec'], mode = options['mode'], vertex = options['vertices'];
      var index = options['indices'], instance = options['instances'];
      var uniforms = options['uniforms'], bounds = options['bounds'];
      function validBuffer(part, type) {
        return part && part['buffer'] instanceof type && !part['buffer']['isDeleted']() &&
               integer(part['count']) && integer(part['offset'] === undefined ? 0 : part['offset']);
      }
      if (!(spec instanceof CanvasKit.MeshSpecification) || spec['isDeleted']() ||
          !validBuffer(vertex, CanvasKit.MeshVertexBuffer) ||
          (index !== undefined && !validBuffer(index, CanvasKit.MeshIndexBuffer)) ||
          (instance !== undefined && !validBuffer(instance, CanvasKit.MeshVertexBuffer)) ||
          (mode !== 'triangles' && mode !== 'triangle-strip') ||
          !(uniforms instanceof Float32Array) || uniforms.byteLength !== spec['uniformSize']() ||
          !bounds || bounds.length !== 4) return null;
      for (var i = 0; i < 4; i++) if (!Number.isFinite(bounds[i])) return null;
      var ptr = CanvasKit._malloc(uniforms.byteLength + 16);
      try {
        CanvasKit.HEAPF32.set(uniforms, ptr / 4);
        CanvasKit.HEAPF32.set(bounds, (ptr + uniforms.byteLength) / 4);
        return CanvasKit._MakeMesh(spec, mode === 'triangles' ? 0 : 1,
          vertex['buffer'], vertex['count'], vertex['offset'] || 0,
          index ? index['buffer'] : null, index ? index['count'] : 0, index ? index['offset'] || 0 : 0,
          ptr, uniforms.byteLength, ptr + uniforms.byteLength,
          instance ? instance['buffer'] : null, instance ? instance['offset'] || 0 : 0,
          instance ? instance['count'] : 0);
      } finally { CanvasKit._free(ptr); }
    };
    CanvasKit.Canvas.prototype.getMeshContext = function() {
      var context = this._getMeshContext();
      if (context) context._context = this._context;
      return context;
    };
    CanvasKit.Canvas.prototype.drawMesh = function(mesh, blender, paint) {
      CanvasKit.setCurrentContext(this._context);
      this._drawMesh(mesh, blender, paint);
    };
  });
}(Module));

/**
 * Xenon's two passes in WebGL2: the plugin's own GLSL, unedited.
 *
 * `shaders.js` is the five `R"( ... )"` pieces of `source/Shaders.cpp` plus
 * kVersion, spliced by `demo/tools/check_shaders.py --write` and never typed;
 * the same script compares them character for character, and checks they are
 * joined here as `Assemble()` and InitGL join them in the plugin.
 *
 *   1. light      one quad per lit head (its bounding box in output pixels,
 *                 padded a pixel) drawn additively into an R32F buffer the size
 *                 of the output; each fragment decides whether its pixel centre
 *                 is inside the head's region by an even-odd crossing count over
 *                 the head's edges in an RGBA32F edge texture.
 *   2. composite  the clip and the light buffer to the output, in linear light.
 *
 * As in the plugin: R32F, blended, needs float blending, which WebGL2 has only
 * as EXT_float_blend (core in desktop GL 4.1), and a float render target
 * (EXT_color_buffer_float, which the kit asks for). Without either the page
 * stops and says so: an 8-bit or half-float light buffer would draw a
 * plausible wrong strobe, which is what the plugin's own notes measured
 * (AGENTS.md: RGBA16F truncated every flash).
 *
 * Everything the passes are handed comes from port.js's Xenon.ProcessFrame:
 * the light per head, the quads, the edges, Band, Colour, Room and Look.
 */

import { Program, PassBuffer, GLError, bindTexture } from './vendor/gl.js';
import * as S from './shaders.js';
import { kVertexFloats, kEdgeRow } from './port.js';

/** The light pass's vertex layout, as InitGL sets it up. */
const LIGHT_ATTRIBS = { vCorner: 0, vRunsIn: 1, vExtraIn: 2 };

export function createPasses(gl) {
  if (!gl.getExtension('EXT_color_buffer_float')) {
    throw new GLError('EXT_color_buffer_float is missing. Xenon adds every flash into a 32-bit float light buffer, as the plugin does, and this browser cannot render into one.');
  }
  if (!gl.getExtension('EXT_float_blend')) {
    throw new GLError('EXT_float_blend is missing. Xenon adds every lit head’s light into a 32-bit float buffer by blending, as the plugin does in desktop GL, and this browser cannot blend into one. An 8-bit or half-float buffer would draw a plausible wrong strobe, so the page stops here instead.');
  }

  // InitGL: each vertex stage is kVersion + its piece; each fragment stage is
  // Assemble( pass ).
  const quadVertex = S.VERSION + S.QUAD_VERTEX;
  const lightVertex = S.VERSION + S.LIGHT_VERTEX;
  const assemble = {
    light: S.VERSION + S.LIGHT_FRAGMENT,
    composite: S.VERSION + S.COMMON + S.COMPOSITE_FRAGMENT,
  };
  const light = new Program(gl, lightVertex, assemble.light, 'light', { attribs: LIGHT_ATTRIBS });
  const composite = new Program(gl, quadVertex, assemble.composite, 'composite');

  // The light pass's quads: a stream buffer refilled every frame with the lit
  // heads' bounding boxes. The VAO holds the layout.
  const vertexArray = gl.createVertexArray();
  const vertexBuffer = gl.createBuffer();
  gl.bindVertexArray(vertexArray);
  gl.bindBuffer(gl.ARRAY_BUFFER, vertexBuffer);
  const stride = kVertexFloats * 4;
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, stride, 0);
  gl.enableVertexAttribArray(1);
  gl.vertexAttribPointer(1, 4, gl.FLOAT, false, stride, 2 * 4);
  gl.enableVertexAttribArray(2);
  gl.vertexAttribPointer(2, 2, gl.FLOAT, false, stride, 6 * 4);
  gl.bindVertexArray(null);
  gl.bindBuffer(gl.ARRAY_BUFFER, null);

  const edgeTexture = gl.createTexture();
  const lightBuffer = new PassBuffer(gl, { filter: 'nearest' });

  return {
    /**
     * Draw one frame. `frame` is what Xenon.ProcessFrame returned; `input` the
     * clip ({ texture }, premultiplied, uv origin bottom left); the output
     * goes to the framebuffer bound when this is called (the canvas), at
     * `width` x `height` from the origin.
     */
    draw(frame, input, width, height, target = null) {
      if (frame.rebuilt) {
        gl.bindTexture(gl.TEXTURE_2D, edgeTexture);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, kEdgeRow, frame.edgeRows, 0, gl.RGBA, gl.FLOAT, frame.edgeTexels);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.bindTexture(gl.TEXTURE_2D, null);
      }
      lightBuffer.ensure(width, height, gl.R32F);

      // 1. The light: every lit head's quad, additively.
      gl.bindFramebuffer(gl.FRAMEBUFFER, lightBuffer.fbo);
      gl.viewport(0, 0, width, height);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      if (frame.vertices.length > 0) {
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.ONE, gl.ONE);
        light.use();
        bindTexture(gl, 0, edgeTexture);
        light.setSampler('Edges', 0);
        light.set('Size', frame.size[0], frame.size[1]);
        light.setInt('Outline', frame.outline);
        light.set('Band', frame.band);
        light.setInt('Hooks', 0);
        gl.bindVertexArray(vertexArray);
        gl.bindBuffer(gl.ARRAY_BUFFER, vertexBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, frame.vertices, gl.STREAM_DRAW);
        gl.drawArrays(gl.TRIANGLES, 0, frame.vertices.length / kVertexFloats);
        gl.bindVertexArray(null);
        gl.bindBuffer(gl.ARRAY_BUFFER, null);
        bindTexture(gl, 0, null);
        gl.disable(gl.BLEND);
      }

      // 2. The clip and the light, to the output.
      gl.bindFramebuffer(gl.FRAMEBUFFER, target);
      gl.viewport(0, 0, width, height);
      composite.use();
      bindTexture(gl, 0, input.texture);
      bindTexture(gl, 1, lightBuffer.texture);
      composite.setSampler('InputTexture', 0);
      composite.setSampler('Light', 1);
      // A browser texture is not padded to a power of two, so MaxUV is 1.
      composite.set('MaxUV', 1.0, 1.0);
      gl.uniform2i(composite.location('ViewOrigin'), 0, 0);
      composite.set('Colour', frame.colour[0], frame.colour[1], frame.colour[2]);
      composite.set('Room', frame.room);
      composite.setInt('Look', frame.look);
      composite.setInt('Hooks', 0);
      this.quad.draw();
      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);
    },

    /** The kit's full-frame quad, set by whoever owns it. */
    quad: null,

    /** Uniform names a program was asked for and does not have (should stay empty). */
    missing() {
      return [...light.missing, ...composite.missing];
    },
  };
}

#version 450 core

// Full-screen triangle from the vertex index alone - no vertex buffer, no
// CPU-side geometry at all. Vertex 0/1/2 land at (0,0)/(2,0)/(0,2) in UV
// space; the part outside the 0..1 square gets clipped away, leaving
// exactly the screen rectangle. Standard technique for a post-process pass
// that has nothing to draw except "the whole target".
layout(location = 0) out vec2 vTexCoord;

void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // Clip space here is Vulkan-style (Y+ down, matching SDL_GPU's other
    // pipeline in this codebase - see shader.vert's own "gl_Position.y *=
    // -1" comment), but the source texture's row 0 is the top row like any
    // normally-loaded image. Flipping V here, once, is what makes "top of
    // clip space" sample "top of the picture" instead of the bottom -
    // confirmed by a visibly upside-down result without it.
    vTexCoord = vec2(uv.x, 1.0 - uv.y);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}

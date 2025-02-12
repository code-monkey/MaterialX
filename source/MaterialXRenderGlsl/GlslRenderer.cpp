//
// TM & (c) 2017 Lucasfilm Entertainment Company Ltd. and Lucasfilm Ltd.
// All rights reserved.  See LICENSE.txt for license.
//

#include <MaterialXRenderGlsl/External/GLew/glew.h>
#include <MaterialXRenderGlsl/GlslRenderer.h>
#include <MaterialXRenderGlsl/GLContext.h>
#include <MaterialXRenderGlsl/GLUtil.h>
#include <MaterialXRenderHw/SimpleWindow.h>
#include <MaterialXRender/TinyObjLoader.h>
#include <MaterialXGenShader/HwShaderGenerator.h>

#include <iostream>

namespace MaterialX
{

const float PI = std::acos(-1.0f);

// View information
const float FOV_PERSP = 45.0f; // degrees
const float NEAR_PLANE_PERSP = 0.05f;
const float FAR_PLANE_PERSP = 100.0f;

//
// GlslRenderer methods
//

GlslRendererPtr GlslRenderer::create(unsigned int width, unsigned int height, Image::BaseType baseType)
{
    return GlslRendererPtr(new GlslRenderer(width, height, baseType));
}

GlslRenderer::GlslRenderer(unsigned int width, unsigned int height, Image::BaseType baseType) :
    ShaderRenderer(width, height, baseType),
    _initialized(false),
    _eye(0.0f, 0.0f, 4.0f),
    _center(0.0f, 0.0f, 0.0f),
    _up(0.0f, 1.0f, 0.0f),
    _objectScale(1.0f),
    _clearColor(0.4f, 0.4f, 0.4f, 1.0f)
{
    _program = GlslProgram::create();

    _geometryHandler = GeometryHandler::create();
    _geometryHandler->addLoader(TinyObjLoader::create());

    _camera = Camera::create();
}

void GlslRenderer::initialize()
{
    if (!_initialized)
    {
        // Create window
        _window = SimpleWindow::create();

        if (!_window->initialize("Renderer Window", _width, _height, nullptr))
        {
            throw ExceptionRenderError("Failed to initialize renderer window");
        }

        // Create offscreen context
        _context = GLContext::create(_window);
        if (!_context)
        {
            throw ExceptionRenderError("Failed to create OpenGL context for renderer");
        }

        if (_context->makeCurrent())
        {
            // Initialize glew
            glewInit();
#if !defined(__APPLE__)
            if (!glewIsSupported("GL_VERSION_4_0"))
            {
                throw ExceptionRenderError("OpenGL version 4.0 is required");
            }
#endif
            glClearStencil(0);

            _frameBuffer = GLFramebuffer::create(_width, _height, 4, _baseType);
            _initialized = true;
        }
    }
}

void GlslRenderer::createProgram(ShaderPtr shader)
{
    if (!_context || !_context->makeCurrent())
    {
        throw ExceptionRenderError("Invalid OpenGL context in createProgram");
    }

    _program->setStages(shader);
    _program->build();
}

void GlslRenderer::createProgram(const StageMap& stages)
{
    if (!_context || !_context->makeCurrent())
    {
        throw ExceptionRenderError("Invalid OpenGL context in createProgram");
    }

    for (const auto& it : stages)
    {
        _program->addStage(it.first, it.second);
    }
    _program->build();
}

void GlslRenderer::renderTextureSpace()
{
    _program->bind();
    _program->bindTextures(_imageHandler);

    _frameBuffer->bind();
    drawScreenSpaceQuad();
    _frameBuffer->unbind();

    _program->unbind();
}

void GlslRenderer::validateInputs()
{
    if (!_context || !_context->makeCurrent())
    {
        throw ExceptionRenderError("Invalid OpenGL context in validateInputs");
    }

    // Check that the generated uniforms and attributes are valid
    _program->getUniformsList();
    _program->getAttributesList();
}

void GlslRenderer::setSize(unsigned int width, unsigned int height)
{
    if (_context->makeCurrent())
    {
        if (_frameBuffer)
        {
            _frameBuffer->resize(width, height);
        }
        else
        {
            _frameBuffer = GLFramebuffer::create(width, height, 4, _baseType);
        }
        _width = width;
        _height = height;
    }
}

void GlslRenderer::updateViewInformation()
{
    float fH = std::tan(FOV_PERSP / 360.0f * PI) * NEAR_PLANE_PERSP;
    float fW = fH * 1.0f;

    _camera->setViewMatrix(Camera::createViewMatrix(_eye, _center, _up));
    _camera->setProjectionMatrix(Camera::createPerspectiveMatrix(-fW, fW, -fH, fH, NEAR_PLANE_PERSP, FAR_PLANE_PERSP));
}

void GlslRenderer::updateWorldInformation()
{
    float aspectRatio = float(_width) / float(_height);
    float geometryRatio = _height < _width ?  aspectRatio : (1.0f / aspectRatio);
    Vector3 boxMin = _geometryHandler->getMinimumBounds();
    Vector3 boxMax = _geometryHandler->getMaximumBounds();
    Vector3 sphereCenter = (boxMax + boxMin) / 2.0;
    float sphereRadius = (sphereCenter - boxMin).getMagnitude() * geometryRatio;
    float meshFit = 2.0f / sphereRadius;
    Vector3 modelTranslation = sphereCenter * -1.0f;
    _camera->setWorldMatrix(Matrix44::createTranslation(modelTranslation) *
                            Matrix44::createScale(Vector3(_objectScale * meshFit)));
}

void GlslRenderer::render()
{
    if (!_context || !_context->makeCurrent())
    {
        throw ExceptionRenderError("Invalid OpenGL context in render");
    }

    // Set up target
    _frameBuffer->bind();

    glClearColor(_clearColor[0], _clearColor[1], _clearColor[2], _clearColor[3]);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_FRAMEBUFFER_SRGB);
    glDepthFunc(GL_LESS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    updateViewInformation();
    updateWorldInformation();

    try
    {
        // Bind program and input parameters
        if (_program)
        {
            // Check if we have any attributes to bind. If not then
            // there is nothing to draw
            if (!_program->hasActiveAttributes())
            {
                throw ExceptionRenderError("Program has no input vertex data");
            }
            else
            {
                // Bind the shader program.
                if (!_program->bind())
                {
                    throw ExceptionRenderError("Cannot bind inputs without a valid program");
                }

                // Update uniforms and attributes.
                _program->getUniformsList();
                _program->getAttributesList();

                // Bind shader properties.
                _program->bindViewInformation(_camera);
                _program->bindTextures(_imageHandler);
                _program->bindLighting(_lightHandler, _imageHandler);
                _program->bindTimeAndFrame();

                // Set blend state for the given material.
                if (_program->getShader()->hasAttribute(HW::ATTR_TRANSPARENT))
                {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                }
                else
                {
                    glDisable(GL_BLEND);
                }

                // Bind each mesh and draw its partitions.
                for (MeshPtr mesh : _geometryHandler->getMeshes())
                {
                    _program->bindMesh(mesh);
                    for (size_t i = 0; i < mesh->getPartitionCount(); i++)
                    {
                        MeshPartitionPtr part = mesh->getPartition(i);
                        _program->bindPartition(part);
                        MeshIndexBuffer& indexData = part->getIndices();
                        glDrawElements(GL_TRIANGLES, (GLsizei)indexData.size(), GL_UNSIGNED_INT, (void*)0);
                    }
                }

                // Unbind resources
                _imageHandler->unbindImages();
                _program->unbind();

                // Restore blend state.
                if (_program->getShader()->hasAttribute(HW::ATTR_TRANSPARENT))
                {
                    glDisable(GL_BLEND);
                }
            }
        }
    }
    catch (ExceptionRenderError& e)
    {
        _frameBuffer->unbind();
        throw e;
    }

    // Unset target
    _frameBuffer->unbind();
}

ImagePtr GlslRenderer::captureImage(ImagePtr image)
{
    return _frameBuffer->getColorImage(image);
}

void GlslRenderer::drawScreenSpaceQuad()
{
    const float QUAD_VERTICES[] =
    {
         1.0f,  1.0f, 0.0f, 1.0f, 1.0f, // position, texcoord
         1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 0.0f, 1.0f
    };
    const unsigned int QUAD_INDICES[] =
    {
        0, 1, 3,
        1, 2, 3
    };
    const unsigned int VERTEX_STRIDE = 5;
    const unsigned int TEXCOORD_OFFSET = 3;

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_VERTICES), QUAD_VERTICES, GL_STATIC_DRAW);

    for (const auto& pair : _program->getAttributesList())
    {
        if (pair.first.find(HW::IN_POSITION) != std::string::npos)
        {
            glEnableVertexAttribArray(pair.second->location);
            glVertexAttribPointer(pair.second->location, 3, GL_FLOAT, GL_FALSE, VERTEX_STRIDE * sizeof(float), (void*) 0);
        }

        if (pair.first.find(HW::IN_TEXCOORD + "_") != std::string::npos)
        {
            glEnableVertexAttribArray(pair.second->location);
            glVertexAttribPointer(pair.second->location, 2, GL_FLOAT, GL_FALSE, VERTEX_STRIDE * sizeof(float), (void*) (TEXCOORD_OFFSET * sizeof(float)));
        }
    }

    GLuint ebo;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(QUAD_INDICES), QUAD_INDICES, GL_STATIC_DRAW);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

    glBindVertexArray(GlslProgram::UNDEFINED_OPENGL_RESOURCE_ID);
    glBindBuffer(GL_ARRAY_BUFFER, GlslProgram::UNDEFINED_OPENGL_RESOURCE_ID);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, GlslProgram::UNDEFINED_OPENGL_RESOURCE_ID);

    glDeleteBuffers(1, &ebo);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);

    checkGlErrors("after draw screen-space quad");
}

void GlslRenderer::setClearColor(const Color4& clearColor)
{
    _clearColor = clearColor;
}

} // namespace MaterialX

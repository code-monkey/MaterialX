//
// TM & (c) 2021 Lucasfilm Entertainment Company Ltd. and Lucasfilm Ltd.
// All rights reserved.  See LICENSE.txt for license.
//

#ifndef MATERIALX_CLOSURESOURCECODEMDL_H
#define MATERIALX_CLOSURESOURCECODEMDL_H

#include <MaterialXGenMdl/Nodes/SourceCodeNodeMdl.h>

namespace MaterialX
{

class MX_GENMDL_API ClosureSourceCodeNodeMdl : public SourceCodeNodeMdl
{
public:
    static ShaderNodeImplPtr create();

    void emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const override;
};

} // namespace MaterialX

#endif

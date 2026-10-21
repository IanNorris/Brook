// Actual upload/preparation/draw/presentation ordering; hosted failure tests share it.
static void CompEndFrameDraw()
{
    FrameTimingScope phase(FramePhase::VertexUpload);
    // 1. Fill the vertex staging buffer: 4 verts/quad, {pos.xy, uv.xy, opacity}.
    //    NDC positions derived from integer pixel rects via NdcBits (no FP).
    int32_t W = static_cast<int32_t>(g_compScanoutW);
    int32_t H = static_cast<int32_t>(g_compScanoutH);
    for (uint32_t i = 0; i < g_drawQuadCount; ++i)
    {
        const DrawQuadRec& q = g_drawQuads[i];
        uint32_t nx0 = NdcBits(q.dx,  W), ny0 = NdcBits(q.dy,  H);
        uint32_t nx1 = NdcBits(q.dx2, W), ny1 = NdcBits(q.dy2, H);
        uint32_t a = q.opacity;
        uint32_t* v = g_drawVtxBuf + i * 20;   // 4 verts * 5 floats
        v[0]=nx0;  v[1]=ny0;  v[2]=q.u0;  v[3]=q.v0;  v[4]=a;
        v[5]=nx1;  v[6]=ny0;  v[7]=q.u1;  v[8]=q.v0;  v[9]=a;
        v[10]=nx0; v[11]=ny1; v[12]=q.u0; v[13]=q.v1; v[14]=a;
        v[15]=nx1; v[16]=ny1; v[17]=q.u1; v[18]=q.v1; v[19]=a;
    }
    uint32_t packetBytes=0;
    {
        FrameTimingScope control(FramePhase::QueueSetup);
        // The transfer owns legacy request/response pages and the vertex data.
        // Only independent compose bytes may change before its completion.
        const bool ready=GpuPrepareWhilePending(
            [&]{return BeginVertexTransfer(COMP_CTX_ID,COMP_DRAW_VBUF_RES,g_drawQuadCount*80);},
            [&]{FrameTimingScope build(FramePhase::CommandBuild);
                packetBytes=BuildComposePacket(g_composeCommand,W,H);return packetBytes!=0;},
            [&](GpuCommandToken token){return CmdRespOk(FinishCommand(token));});
        if (!ready) return;
    }

    // The display consumer is a different host GL context. Publish only after
    // rendering completes; a plain used-ring ack is not a GPU-completion fence.
    phase.Change(FramePhase::CommandBuild);
    uint32_t responseBytes;
    {
        FrameTimingScope control(FramePhase::QueueSetup);
        responseBytes=FinishCommand(BeginCommand(g_composeCommand,packetBytes,CMD_RESP_CAP,true));
    }
    if (!CmdRespOk(g_composeCommand,responseBytes))
        return;
    g_drawQuadCount = 0;
    g_blurBarrierCount = 0;
    phase.Change(FramePhase::DisplayFlush);
    ResourceFlush(COMP_SCANOUT_RES, 0, 0, g_compScanoutW, g_compScanoutH);
}

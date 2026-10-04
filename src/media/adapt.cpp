#include "media/adapt.hpp"

#include <cmath>
#include <sstream>

namespace srtgw
{
namespace
{
bool nearHz(double a, double b)
{
    if (a <= 0.0 || b <= 0.0)
    {
        return false;
    }
    return std::abs(a - b) / a < 0.02;
}

Deint pickDeint(std::string const& name, bool fieldRate)
{
    if (name == "weave" || name == "none")
    {
        return fieldRate ? Deint::WeaveField : Deint::WeaveFrame;
    }
    if (name == "yadif")
    {
        return fieldRate ? Deint::YadifField : Deint::YadifFrame;
    }
    return fieldRate ? Deint::BwdifField : Deint::BwdifFrame;
}

std::string parity(std::string const& order)
{
    return order == "bff" ? "bff" : "tff";
}
} // namespace

std::string deintName(Deint deint)
{
    switch (deint)
    {
    case Deint::BwdifField:
        return "bwdif-field";
    case Deint::BwdifFrame:
        return "bwdif-frame";
    case Deint::YadifField:
        return "yadif-field";
    case Deint::YadifFrame:
        return "yadif-frame";
    case Deint::WeaveField:
        return "weave-field";
    case Deint::WeaveFrame:
        return "weave-frame";
    case Deint::None:
        return "none";
    }
    return "none";
}

AdaptationPlan planAdaptation(VideoFormat source, VideoFormat const& target, std::string const& deinterlacer, std::string const& aspect,
    std::string const& kernel, std::string const& sourceScan)
{
    if (sourceScan == "progressive")
    {
        source.interlaced = false;
        source.fieldOrder = "progressive";
    }
    else if (sourceScan == "tff" || sourceScan == "bff")
    {
        source.interlaced = true;
        source.fieldOrder = sourceScan;
    }

    AdaptationPlan plan;
    plan.aspect = aspect == "fill" ? "fill" : "letterbox";
    plan.kernel = kernel == "lanczos" ? "lanczos" : "bicubic";
    plan.output = target;
    plan.targetFieldOrder = target.fieldOrder == "bff" ? "bff" : "tff";
    plan.anamorphic = source.sarDen > 0 && source.sarNum > 0 && source.sarNum != source.sarDen;

    bool const srcInterlaced = source.interlaced;
    bool const dstInterlaced = target.interlaced;
    double const srcFrame = source.rate.hz();
    double const srcField = source.fieldHz();
    double const dstFrame = target.rate.hz();
    double const dstField = target.fieldHz();

    if (srcInterlaced && !dstInterlaced)
    {
        bool const toFieldRate = nearHz(dstFrame, srcField);
        bool const toFrameRate = nearHz(dstFrame, srcFrame);
        plan.deint = pickDeint(deinterlacer, toFieldRate || !toFrameRate);
    }
    else if (!srcInterlaced && dstInterlaced)
    {
        if (nearHz(srcFrame, dstField))
        {
            plan.lace = Lace::TwoFrames;
        }
        else
        {
            plan.lace = Lace::Psf;
        }
    }
    else if (srcInterlaced && dstInterlaced && source.fieldOrder != plan.targetFieldOrder && source.fieldOrder != "progressive")
    {
        plan.fieldShift = true;
    }

    if (source.width != target.width || source.height != target.height || plan.anamorphic)
    {
        plan.scale = true;
    }

    bool const sd = source.height > 0 && source.height <= 576;
    bool const src601 = source.color == "bt601" || source.color == "smpte170m" || (sd && source.color != "bt709");
    bool const dstHd = target.height >= 720;
    if (src601 && dstHd)
    {
        plan.color = true;
    }

    if (!nearHz(srcInterlaced && !dstInterlaced && plan.deint == Deint::BwdifField ? srcField : srcFrame, dstInterlaced ? dstFrame : dstFrame) ||
        !source.rate.sameAs(target.rate))
    {
        // Field-rate deinterlace already changes the frame rate. Anything still
        // left (59.94 → 50, and the like) is repeat/drop in the synchroniser.
        bool const deintChangesRate = srcInterlaced && !dstInterlaced &&
                                      (plan.deint == Deint::BwdifField || plan.deint == Deint::YadifField || plan.deint == Deint::WeaveField);
        bool const laceChangesRate = plan.lace == Lace::TwoFrames;
        double const produced = deintChangesRate ? srcField : (laceChangesRate ? srcFrame / 2.0 : srcFrame);
        plan.rateBySync = !nearHz(produced, dstFrame);
    }

    std::ostringstream summary;
    summary << source.label() << " -> " << target.label() << " deint=" << deintName(plan.deint);
    if (plan.lace == Lace::TwoFrames)
    {
        summary << " lace=fields";
    }
    else if (plan.lace == Lace::Psf)
    {
        summary << " lace=psf";
    }
    if (plan.scale)
    {
        summary << " scale";
    }
    if (plan.color)
    {
        summary << " color";
    }
    if (plan.fieldShift)
    {
        summary << " fieldshift";
    }
    if (plan.rateBySync)
    {
        summary << " sync";
    }
    plan.summary = summary.str();
    return plan;
}

std::string ffmpegFilter(AdaptationPlan const& plan, bool cuda, bool keepCuda)
{
    std::string chain;
    auto add = [&](std::string const& step) {
        if (!chain.empty())
        {
            chain += ",";
        }
        chain += step;
    };
    // `cuda` means the buffersrc frame is AV_PIX_FMT_CUDA. CPU-only filters
    // download first. scale_cuda keeps aspect; pad/crop run after the download.
    bool onCuda = cuda;
    auto download = [&]() {
        if (onCuda)
        {
            add("hwdownload,format=nv12");
            onCuda = false;
        }
    };
    auto scaleAlgo = [](std::string const& kernel) {
        if (kernel == "lanczos" || kernel == "bilinear" || kernel == "nearest" || kernel == "bicubic")
        {
            return kernel;
        }
        return std::string("bicubic");
    };

    std::string const order = parity(plan.output.fieldOrder.empty() ? "tff" : plan.targetFieldOrder);
    std::string const mode = (plan.deint == Deint::BwdifField || plan.deint == Deint::YadifField || plan.deint == Deint::WeaveField) ? "send_field" : "send_frame";
    if (onCuda && (plan.deint == Deint::BwdifField || plan.deint == Deint::BwdifFrame))
    {
        add("bwdif_cuda=mode=" + mode + ":parity=" + order);
    }
    else if (onCuda && (plan.deint == Deint::YadifField || plan.deint == Deint::YadifFrame))
    {
        add("yadif_cuda=mode=" + mode + ":parity=" + order);
    }
    else if (plan.deint == Deint::BwdifField || plan.deint == Deint::BwdifFrame)
    {
        download();
        add("bwdif=mode=" + mode + ":parity=" + order);
    }
    else if (plan.deint == Deint::YadifField || plan.deint == Deint::YadifFrame)
    {
        download();
        add("yadif=mode=" + mode + ":parity=" + order);
    }
    else if (plan.deint == Deint::WeaveFrame)
    {
        download();
        add(std::string("separatefields,weave=first_field=") + (order == "bff" ? "bottom" : "top"));
    }
    else if (plan.deint == Deint::WeaveField)
    {
        download();
        add("separatefields,scale=iw:ih*2:flags=neighbor");
    }

    if (plan.fieldShift)
    {
        download();
        add(std::string("fieldorder=") + order);
    }

    if (plan.anamorphic)
    {
        download();
        add("scale=iw*sar:ih,setsar=1");
    }

    if (plan.scale)
    {
        std::string const wh = std::to_string(plan.output.width) + ":" + std::to_string(plan.output.height);
        if (onCuda)
        {
            std::string const fit = plan.aspect == "fill" ? "increase" : "decrease";
            add("scale_cuda=" + wh + ":force_original_aspect_ratio=" + fit + ":interp_algo=" + scaleAlgo(plan.kernel));
            download();
            if (plan.aspect == "fill")
            {
                add("crop=" + wh);
            }
            else
            {
                add("pad=" + wh + ":(ow-iw)/2:(oh-ih)/2:color=black");
            }
        }
        else if (plan.aspect == "fill")
        {
            add("scale=" + wh + ":force_original_aspect_ratio=increase:flags=" + plan.kernel + ",crop=" + wh);
        }
        else
        {
            add("scale=" + wh + ":force_original_aspect_ratio=decrease:flags=" + plan.kernel + ",pad=" + wh + ":(ow-iw)/2:(oh-ih)/2:color=black");
        }
    }

    if (plan.color)
    {
        download();
        add("colorspace=all=bt709:iall=bt601:fast=1");
    }

    if (plan.lace == Lace::TwoFrames)
    {
        download();
        add(std::string("tinterlace=mode=") + (order == "bff" ? "interleave_bottom" : "interleave_top"));
    }
    else if (plan.lace == Lace::Psf)
    {
        download();
        add(std::string("split[psf_a][psf_b];[psf_a][psf_b]interleave,tinterlace=mode=") + (order == "bff" ? "interleave_bottom" : "interleave_top"));
    }

    if (keepCuda && onCuda)
    {
        // Every step ran on the GPU: the caller converts the CUDA frame to v210 there.
        return chain.empty() ? "null" : chain;
    }
    download();
    add("format=yuv422p10le");
    return chain;
}
} // namespace srtgw

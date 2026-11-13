#include "infer/infer_worker.h"

#include <chrono>

namespace mb {


InferWorker::~InferWorker() { stop(); }

void InferWorker::start(RknnEngine* det, RknnEngine* pose, RknnEngine* fire) {
    stop();
    det_ = det;
    pose_ = pose;
    fire_ = fire;
    {
        std::lock_guard<std::mutex> lk(mu_);
        running_ = true;
        has_job_ = has_result_ = false;
    }
    busy_ = false;
    th_ = std::thread(&InferWorker::loop, this);
}

void InferWorker::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    busy_ = false;
}

bool InferWorker::submit(InferJob job) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_ || busy_) return false;
        job_ = std::move(job);
        has_job_ = true;
        busy_ = true;
    }
    cv_.notify_one();
    return true;
}

bool InferWorker::poll(InferResult* out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!has_result_) return false;
    *out = std::move(result_);
    has_result_ = false;
    return true;
}

void InferWorker::run_job(const InferJob& job, InferResult* r) {
    const auto t0 = std::chrono::steady_clock::now();
    const ImageRGB& img = *job.img;
    r->ts_ms = job.ts_ms;
    r->img_w = img.width;
    r->img_h = img.height;
    std::string err;

    if (job.run_det && det_) {
        det_in_.width = det_->input_width();
        det_in_.height = det_->input_height();
        Letterbox lb;
        const char* warn = nullptr;
        if (!letterbox_rgb(img, &det_in_, &lb, job.pre, &warn)) {
            r->error = "检测 letterbox 失败";
        } else if (!det_->run(det_in_.data.data(), det_in_.size_bytes(), &err)) {
            r->error = "检测推理失败: " + err;
        } else if (!yolo_postprocess(*det_, job.yolo, lb, &r->dets, &err)) {
            r->error = "检测后处理失败: " + err;
        } else {
            r->has_det = true;
        }
    }

    if (job.run_pose && pose_ && pose_->ok()) {
        err.clear();
        if (pose_runner_.run(*pose_, img, r->has_det ? &r->dets : nullptr, job.pose_hints,
                             job.pose, job.pose_roi, job.pre, &r->poses, &err)) {
            r->has_pose = true;
        } else {
            r->error += (r->error.empty() ? "" : "; ") + ("姿态推理/解码失败: " + err);
        }
    }

    if (job.run_fire && fire_ && fire_->ok()) {
        err.clear();
        fire_in_.width = fire_->input_width();
        fire_in_.height = fire_->input_height();
        Letterbox lb;
        const char* warn = nullptr;
        std::string ferr;
        if (!letterbox_rgb(img, &fire_in_, &lb, job.pre, &warn)) {
            ferr = "火焰 letterbox 失败";
        } else if (!fire_->run(fire_in_.data.data(), fire_in_.size_bytes(), &err)) {
            ferr = "火焰推理失败: " + err;
        } else if (!yolo_postprocess(*fire_, job.fire, lb, &r->fire_dets, &err)) {
            ferr = "火焰后处理失败: " + err;
        } else {
            r->has_fire = true;
        }
        if (!ferr.empty()) r->error += (r->error.empty() ? "" : "; ") + ferr;
    }

    r->ai_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                   .count();
}

void InferWorker::loop() {
    while (true) {
        InferJob job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return !running_ || has_job_; });
            if (!running_) return;
            job = std::move(job_);
            has_job_ = false;
        }
        InferResult r;
        run_job(job, &r);
        {
            std::lock_guard<std::mutex> lk(mu_);
            result_ = std::move(r);
            has_result_ = true;
            busy_ = false;
        }
    }
}

}  // namespace mb

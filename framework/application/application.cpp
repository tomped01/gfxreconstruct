/*
** Copyright (c) 2018 Valve Corporation
** Copyright (c) 2018 LunarG, Inc.
**
** Permission is hereby granted, free of charge, to any person obtaining a
** copy of this software and associated documentation files (the "Software"),
** to deal in the Software without restriction, including without limitation
** the rights to use, copy, modify, merge, publish, distribute, sublicense,
** and/or sell copies of the Software, and to permit persons to whom the
** Software is furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
** FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
*/

#include "application/application.h"

#include "util/logging.h"

#include <algorithm>
#include <cassert>

GFXRECON_BEGIN_NAMESPACE(gfxrecon)
GFXRECON_BEGIN_NAMESPACE(application)

Application::Application(const std::string& name) :
    file_processor_(nullptr), running_(false), paused_(false), name_(name)
{}

Application::~Application()
{
    if (!windows_.empty())
    {
        GFXRECON_LOG_INFO(
            "Application manager is destroying windows that were not previously destroyed by their owner");

        for (auto window : windows_)
        {
            delete window;
        }
    }
}

void Application::SetFileProcessor(decode::FileProcessor* file_processor)
{
    file_processor_ = file_processor;
}

void Application::Run(uint32_t measurement_start_frame,
                      uint32_t measurement_end_frame,
                      bool     quit_after_range,
                      bool     flush_measurement_range)
{
    running_               = true;
    measurement_start_time = 0;
    measurement_end_time   = 0;

    while (running_)
    {
        ProcessEvents(paused_);

        // Only process the next frame if a quit event was not processed or not paused.
        if (running_ && !paused_)
        {
            HandleMeasurementRange(
                measurement_start_frame, measurement_end_frame, quit_after_range, flush_measurement_range);

            if (running_)
            {
                PlaySingleFrame();
            }
        }
    }

    WriteMeasurementRangeFpsToConsole(measurement_start_frame, measurement_end_frame);
}

void Application::SetPaused(bool paused)
{

    paused_ = paused;

    if (paused_ && (file_processor_ != nullptr))
    {
        uint32_t current_frame = file_processor_->GetCurrentFrameNumber();
        if (current_frame > 0)
        {
            GFXRECON_LOG_INFO("Paused at frame %u", file_processor_->GetCurrentFrameNumber());
        }
    }
}

bool Application::PlaySingleFrame()
{
    bool success = false;

    if (file_processor_)
    {
        success = file_processor_->ProcessNextFrame();

        if (success)
        {
            if (file_processor_->GetCurrentFrameNumber() == pause_frame_)
            {
                paused_ = true;
            }

            // Check paused state separately from previous check to print messages for two different cases: replay has
            // paused on the user specified pause frame (tested above), or the user has pressed a key to advance forward
            // by one frame while paused.
            if (paused_)
            {
                GFXRECON_LOG_INFO("Paused at frame %u", file_processor_->GetCurrentFrameNumber());
            }
        }
        else
        {
            running_ = false;
        }
    }

    return success;
}

bool Application::RegisterWindow(decode::Window* window)
{
    assert(window != nullptr);

    if (std::find(windows_.begin(), windows_.end(), window) != windows_.end())
    {
        GFXRECON_LOG_INFO("A window was registered with the application more than once");
        return false;
    }

    windows_.push_back(window);

    return true;
}

bool Application::UnregisterWindow(decode::Window* window)
{
    assert(window != nullptr);

    auto pos = std::find(windows_.begin(), windows_.end(), window);

    if (pos == windows_.end())
    {
        GFXRECON_LOG_INFO(
            "A remove window request was made for an window that was never registered with the application");
        return false;
    }

    windows_.erase(pos);

    return true;
}

void Application::HandleMeasurementRange(uint32_t measurement_start_frame,
                                         uint32_t measurement_end_frame,
                                         bool     quit_after_range,
                                         bool     flush_measurement_range)
{
    if (file_processor_->GetCurrentFrameNumber() == measurement_start_frame)
    {
        if (flush_measurement_range)
        {
            file_processor_->WaitDecodersIdle();
        }

        measurement_start_time = gfxrecon::util::datetime::GetTimestamp();
    }
    else if (file_processor_->GetCurrentFrameNumber() == measurement_end_frame)
    {
        // End before replay -> non inclusive range
        if (flush_measurement_range)
        {
            file_processor_->WaitDecodersIdle();
        }

        measurement_end_time = gfxrecon::util::datetime::GetTimestamp();

        if (quit_after_range)
        {
            running_ = false;
        }
    }
}

void Application::WriteMeasurementRangeFpsToConsole(uint32_t measurement_start_frame, uint32_t measurement_end_frame)
{
    if (file_processor_->GetErrorState() != gfxrecon::decode::FileProcessor::kErrorNone)
    {
        GFXRECON_LOG_ERROR("A failure has occurred during replay, cannot calculate measurement range FPS.");
        return;
    }

    if (running_ && (file_processor_->GetCurrentFrameNumber() < measurement_end_frame))
    {
        GFXRECON_LOG_WARNING("Application is still running and has not yet reached the measurement "
                             "range end frame. Cannot calculate measurement range FPS.")
        return;
    }

    if (measurement_start_frame >= measurement_end_frame)
    {
        GFXRECON_LOG_WARNING("Measurement start frame (%u) is greater than or equal to the end frame (%u). "
                             "Cannot calculate measurement range FPS.",
                             measurement_start_frame,
                             measurement_end_frame);

        return;
    }

    if (file_processor_->GetCurrentFrameNumber() < measurement_start_frame)
    {
        GFXRECON_LOG_WARNING("Measurement range start frame (%u) is greater than the last replayed frame (%u). "
                             "Measurements were never started, cannot calculate measurement range FPS.",
                             measurement_start_frame,
                             file_processor_->GetCurrentFrameNumber());
        return;
    }

    // Here we clip the range for convenience.
    if (file_processor_->GetCurrentFrameNumber() < measurement_end_frame)
    {
        file_processor_->WaitDecodersIdle();
        measurement_end_time  = gfxrecon::util::datetime::GetTimestamp();
        measurement_end_frame = file_processor_->GetCurrentFrameNumber();
    }

    double diff_time_sec = gfxrecon::util::datetime::ConvertTimestampToSeconds(
        gfxrecon::util::datetime::DiffTimestamps(measurement_start_time, measurement_end_time));

    uint32_t total_frames = measurement_end_frame - measurement_start_frame;
    double   fps          = static_cast<double>(total_frames) / diff_time_sec;
    GFXRECON_WRITE_CONSOLE("Measurement range FPS: %f fps, %f seconds, %u frame%s, 1 loop, framerange [%u-%u)",
                           fps,
                           diff_time_sec,
                           total_frames,
                           total_frames > 1 ? "s" : "",
                           measurement_start_frame,
                           measurement_end_frame);
}

GFXRECON_END_NAMESPACE(application)
GFXRECON_END_NAMESPACE(gfxrecon)

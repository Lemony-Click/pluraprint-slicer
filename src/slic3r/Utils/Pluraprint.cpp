#include "Pluraprint.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <mutex>
#include <condition_variable>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/filesystem.hpp>

#include <wx/app.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

Pluraprint::Pluraprint(DynamicPrintConfig *config)
    : m_host(config->opt_string("print_host"))
    , m_apikey(config->opt_string("printhost_apikey"))
    , m_cafile(config->opt_string("printhost_cafile"))
    , m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* Pluraprint::get_name() const { return "Pluraprint"; }

bool Pluraprint::test(wxString &msg) const
{
    // Validate connection by hitting the ingest endpoint with a GET/HEAD-like check.
    // The Pluraprint server should respond to the base URL or a health endpoint.
    const char *name = get_name();
    bool res = true;
    auto url = make_url("api/ingest/pluraprint-slicer-3mf");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Testing connection at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error testing connection: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Connection test successful: HTTP %2%") % name % status;
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    return res;
}

wxString Pluraprint::get_test_ok_msg() const
{
    return _(L("Connection to Pluraprint is working correctly."));
}

wxString Pluraprint::get_test_failed_msg(wxString &msg) const
{
    return GUI::format_wxstr("%s: %s"
        , _L("Could not connect to Pluraprint")
        , msg);
}

bool Pluraprint::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    const char *name = get_name();

    // Export a combined 3MF containing full project data (geometry, settings)
    // AND the embedded sliced GCODE for the active plate.
    fs::path combined_3mf_path;
    if (!export_combined_project_3mf(combined_3mf_path, error_fn)) {
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Combined project+gcode 3MF exported to: %2%") % name % combined_3mf_path.string();

    // Upload the single combined 3MF to the ingest endpoint.
    // POST {server_url}/api/ingest/pluraprint-slicer-3mf
    // The archive contains both the editable project (geometry, transforms,
    // modifiers, settings) and the sliced GCODE — one file, one upload.
    const auto upload_filename = upload_data.upload_path.filename();
    std::string url = make_url("api/ingest/pluraprint-slicer-3mf");
    bool result = true;

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading combined 3MF to %2%") % name % url;

    auto http = Http::post(std::move(url));
    set_auth(http);
    http.form_add_file("project_file", combined_3mf_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Upload complete: HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading to %2%: %3%, HTTP %4%, body: `%5%`")
                % name % url % error % status % body;
            error_fn(format_error(body, error, status));
            result = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                result = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    // Clean up the temp combined 3MF file.
    boost::system::error_code ec;
    fs::remove(combined_3mf_path, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Failed to remove temp file %2%: %3%")
            % name % combined_3mf_path.string() % ec.message();
    }

    return result;
}

bool Pluraprint::export_combined_project_3mf(fs::path &out_path, ErrorFn error_fn) const
{
    const char *name = get_name();

    // Generate a temp file path for the combined project+gcode 3MF.
    out_path = fs::temp_directory_path() / fs::unique_path("pluraprint-combined-%%%%-%%%%-%%%%-%%%%.3mf");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Exporting combined project+gcode 3MF to: %2%") % name % out_path.string();

    // export_3mf() must be called on the UI thread because it accesses
    // the Plater, Model, preset bundle, and OpenGL context for thumbnails.
    // Since PrintHost::upload() runs on the PrintHostJobQueue background thread,
    // we dispatch via CallAfter + condition_variable to block until complete.

    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    int export_result = -1;

    wxTheApp->CallAfter([&]() {
        try {
            auto *plater = GUI::wxGetApp().plater();
            if (!plater) {
                export_result = -1;
            } else {
                // SaveStrategy::Silence   — don't update the project filename in the title bar.
                // SaveStrategy::SplitModel — use production extension format (standard for project saves).
                // SaveStrategy::ShareMesh  — share mesh data between instances for smaller file size.
                // SaveStrategy::WithGcode  — embed sliced GCODE for plates that have been sliced.
                // SaveStrategy::Zip64      — support large files.
                // NO SkipModel — full mesh geometry is embedded alongside the GCODE.
                // This produces a single 3MF with:
                //   3D/3dmodel.model, Metadata/model_settings.config,
                //   Metadata/project_settings.config, Metadata/slice_info.config,
                //   Metadata/plate_N.gcode (for sliced plates),
                //   Metadata/plate_N.json, [Content_Types].xml, _rels, etc.
                SaveStrategy strategy = SaveStrategy::Silence
                                      | SaveStrategy::SplitModel
                                      | SaveStrategy::ShareMesh
                                      | SaveStrategy::WithGcode
                                      | SaveStrategy::Zip64;
                export_result = plater->export_3mf(out_path, strategy, -1 /* all plates */, nullptr);
            }
        } catch (const std::exception &ex) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Exception during combined export: %2%") % name % ex.what();
            export_result = -1;
        }

        {
            std::lock_guard<std::mutex> lk(mtx);
            done = true;
        }
        cv.notify_one();
    });

    // Block the upload thread until the UI thread finishes the export.
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&done]{ return done; });
    }

    if (export_result != 0) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Combined 3MF export failed (result=%2%)") % name % export_result;
        error_fn(_L("Failed to export combined project+gcode 3MF for Pluraprint upload."));
        return false;
    }

    return true;
}

bool Pluraprint::upload_file(const std::string &endpoint,
                             const std::string &field_name,
                             const fs::path &file_path,
                             const std::string &remote_filename,
                             ProgressFn prorgess_fn,
                             ErrorFn error_fn) const
{
    const char *name = get_name();
    std::string url = make_url(endpoint);
    bool result = true;

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading %2% to %3%, filename: %4%")
        % name % field_name % url % remote_filename;

    auto http = Http::post(std::move(url));
    set_auth(http);
    http.form_add_file(field_name, file_path.string(), remote_filename)
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << boost::format("%1%: File uploaded (%2%): HTTP %3%: %4%")
                % name % field_name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading %2%: %3%, HTTP %4%, body: `%5%`")
                % name % field_name % error % status % body;
            error_fn(format_error(body, error, status));
            result = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                result = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    return result;
}

void Pluraprint::set_auth(Http &http) const
{
    http.header("X-Api-Key", m_apikey);

    if (!m_cafile.empty()) {
        http.ca_file(m_cafile);
    }
}

std::string Pluraprint::make_url(const std::string &path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/') {
            return (boost::format("%1%%2%") % m_host % path).str();
        } else {
            return (boost::format("%1%/%2%") % m_host % path).str();
        }
    } else {
        return (boost::format("http://%1%/%2%") % m_host % path).str();
    }
}

} // namespace Slic3r

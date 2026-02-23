#ifndef slic3r_Pluraprint_hpp_
#define slic3r_Pluraprint_hpp_

#include <string>
#include <wx/string.h>
#include <boost/optional.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Http;

// Pluraprint print host: exports a combined 3MF containing both the full project
// data (embedded geometry, settings) and the sliced GCODE for the active plate,
// then uploads this single archive to a Pluraprint server via HTTP multipart/form-data.
class Pluraprint : public PrintHost
{
public:
    Pluraprint(DynamicPrintConfig *config);
    ~Pluraprint() override = default;

    const char* get_name() const override;

    bool test(wxString &curl_msg) const override;
    wxString get_test_ok_msg() const override;
    wxString get_test_failed_msg(wxString &msg) const override;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::None; }
    std::string get_host() const override { return m_host; }

private:
    std::string m_host;
    std::string m_apikey;
    std::string m_cafile;
    bool        m_ssl_revoke_best_effort;

    void set_auth(Http &http) const;
    std::string make_url(const std::string &path) const;

    // Export a combined project 3MF (with geometry + embedded GCODE) to a temp file.
    // Must be dispatched to the UI thread since export_3mf uses OpenGL.
    // Returns true on success, filling out_path with the temp file path.
    bool export_combined_project_3mf(boost::filesystem::path &out_path, ErrorFn error_fn) const;

    // Upload a single file to the given endpoint via multipart/form-data.
    bool upload_file(const std::string &endpoint,
                     const std::string &field_name,
                     const boost::filesystem::path &file_path,
                     const std::string &remote_filename,
                     ProgressFn prorgess_fn,
                     ErrorFn error_fn) const;
};

} // namespace Slic3r

#endif // slic3r_Pluraprint_hpp_

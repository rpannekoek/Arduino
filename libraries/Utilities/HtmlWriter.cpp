#include <HtmlWriter.h>
#include <math.h>

// Constructor
HtmlWriter::HtmlWriter(StringBuilder& output, const char* icon, const char* css, size_t maxBarLength)
    : _output(output), _icon(icon), _css(css), _maxBarLength(maxBarLength)
{
    _titlePrefix = "ESP";
    _bar = new char[maxBarLength + 1]; 
}


// Destructor
HtmlWriter::~HtmlWriter()
{
    delete[] _bar;
}


void HtmlWriter::setTitlePrefix(const char* prefix)
{
    _titlePrefix = prefix;
}


void HtmlWriter::writeHeader(String title, bool includeHomePageLink, bool includeHeading, uint16_t refreshInterval)
{
    _output.clear();
    _output.println(F("<html>"));
    
    _output.println(F("<head>"));
    _output.printf(F("<title>%s - %s</title>\r\n"), _titlePrefix, title.c_str());
    _output.printf(F("<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">\r\n"), _css);
    _output.printf(F("<link rel=\"icon\" sizes=\"128x128\" href=\"%s\">\r\n<link rel=\"apple-touch-icon-precomposed\" sizes=\"128x128\" href=\"%s\">\r\n"), _icon, _icon);
    if (refreshInterval > 0)
        _output.printf(F("<meta http-equiv=\"refresh\" content=\"%d\">\r\n") , refreshInterval);
    _output.println(F("</head>"));
    
    _output.println(F("<body>"));
    if (includeHomePageLink)
        _output.printf(F("<a href=\"/\"><img src=\"%s\"></a>"), _icon);
    if (includeHeading)
        _output.printf(F("<h1>%s</h1>\r\n"), title.c_str());
}


void HtmlWriter::writeFooter()
{
    _output.println(F("</body>"));
    _output.println(F("</html>"));
}


void HtmlWriter::writeBar(float value, String cssClass, bool fill)
{
    int barLength = round(value * _maxBarLength);
    if (barLength > _maxBarLength) barLength = _maxBarLength;

    memset(_bar, 'o', barLength);
    _bar[barLength] = 0;

    _output.printf(F("<div><span class=\"%s\">%s</span>"), cssClass.c_str(), _bar);

    if (fill)
    {
        memset(_bar, 'o', _maxBarLength - barLength);
        _bar[_maxBarLength - barLength] = 0;

        _output.printf(F("<span class=\"barFill\">%s</span>"), _bar);
    }
    else if (barLength == 0)
    {
        // Ensure that an empty bar has the same height
        _output.print(F("<span class=\"emptyBar\">o</span>"));
    }

    _output.print("</div>");
}


void HtmlWriter::writeTextBox(String name, String label, String value,  uint16_t maxLength)
{
    _output.printf(
        F("<tr><td><label for=\"%s\">%s</label></td><td><input type=\"text\" name=\"%s\" value=\"%s\" maxlength=\"%d\"></td></tr>\r\n"), 
        name.c_str(),
        label.c_str(),
        name.c_str(),
        value.c_str(),
        maxLength
        );
}


void HtmlWriter::writeCheckbox(String name, String label, bool value)
{
    const char* checked = value ? "checked" : "";

    _output.printf(
        F("<tr><td><label for=\"%s\">%s</label></td><td><input type=\"checkbox\" name=\"%s\" value=\"true\" %s></td></tr>\r\n"), 
        name.c_str(),
        label.c_str(),
        name.c_str(),
        checked
        );
}


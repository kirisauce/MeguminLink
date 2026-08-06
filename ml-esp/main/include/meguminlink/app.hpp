#ifndef __MEGUMINLINK_APP_HPP__
#define __MEGUMINLINK_APP_HPP__

namespace mlink
{
enum class ApplicationState
{
    Idle,
    Pairing,
};

class Application
{
  public:
    Application();

    ApplicationState state() const;
    void set_state(ApplicationState new_state);

    void run();

  private:
    ApplicationState m_state;
};
} // namespace mlink

#endif
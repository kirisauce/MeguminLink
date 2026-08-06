#include "meguminlink/app.hpp"
#include "meguminlink/pairing.hpp"

using namespace mlink;

Application::Application() : m_state(ApplicationState::Idle)
{
}

ApplicationState Application::state() const
{
    return m_state;
}

void Application::set_state(ApplicationState new_state)
{
    m_state = new_state;
}

void Application::run()
{
    switch (state())
    {
    case ApplicationState::Idle: {
    }
    case ApplicationState::Pairing: {
        pairing_task();
    }
    }
}
#include "meguminlink/app.hpp"
#include "meguminlink/pairing.hpp"

using namespace mlink;

Application::Application() : m_state(ApplicationState::Idle), m_main_task_wake(xSemaphoreCreateBinary())
{
}

ApplicationState Application::state() const
{
    return m_state;
}

void Application::set_state(ApplicationState new_state)
{
    m_state = new_state;
    xSemaphoreGive(main_task_wake());
}

void Application::set_state_from_isr(ApplicationState new_state, BaseType_t *const pxHigherPriorityTaskWoken)
{
    m_state = new_state;
    xSemaphoreGiveFromISR(main_task_wake(), pxHigherPriorityTaskWoken);
}

SemaphoreHandle_t Application::main_task_wake()
{
    return m_main_task_wake;
}

void Application::run()
{
    while (true)
    {
        xSemaphoreTake(main_task_wake(), pdMS_TO_TICKS(200));

        switch (state())
        {
        case ApplicationState::Idle: {
            break;
        }
        case ApplicationState::Pairing: {
            pairing_process(*this);
            break;
        }
        }
    }
}

static Application global_app;

Application &Application::get_instance()
{
    return global_app;
}